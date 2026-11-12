//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Streaming tar walker over the shared read seam: probe / open / next /
//! read. Ported from src/unarch_tar.c (RA8FW-1197) behind the unchanged
//! declarations in inc/unarch_tar.h -- the C ABI callers and the host tests
//! drive stays exactly as declared. Clean-room: no code from GNU tar /
//! libarchive / busybox.

const std = @import("std");
const fields = @import("unarch_tar_fields_abi.zig");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, C23 enum : uint16_t) ---------

const k_ra8_ok: u16 = 0;
const k_ra8_err_no_mem: u16 = 0x102;
const k_ra8_err_invalid_state: u16 = 0x104;
const k_ra8_err_invalid_size: u16 = 0x105;
const k_ra8_err_not_found: u16 = 0x106;
const k_ra8_err_not_supported: u16 = 0x107;
const k_ra8_err_validation_failed: u16 = 0x501;
const k_ra8_err_null_ptr: u16 = 0x504;

// ---- unarch_tar_dims_t (inc/unarch_tar.h, C23 enum : uint16_t) ------------

const k_unarch_tar_block: usize = 512;
const k_unarch_tar_meta_max: u32 = 4;
const k_unarch_tar_pax_max: usize = 2048;

// ---- The decompression-policy seam (libs/ra8_core) ------------------------

/// ra8_decomp_limits_t mirror (libs/ra8_core/src/internal/decomp/policy.zig).
pub const Limits = extern struct {
    max_output_bytes: u64 = 0,
    max_ratio: u32 = 0,
    ratio_grace_bytes: u32 = 0,
    max_entries: u32 = 0,
    max_iterations: u32 = 0,
    max_depth: u8 = 0,
};

/// ra8_decomp_budget_t mirror (libs/ra8_core/src/internal/decomp/budget.zig).
pub const Budget = extern struct {
    limits: Limits = .{},
    out_bytes: u64 = 0,
    in_bytes: u64 = 0,
    entries: u32 = 0,
    iters: u32 = 0,
    depth: u8 = 0,
};

extern fn ra8_decomp_budget_init(b: ?*Budget, limits: ?*const Limits) callconv(.c) c_int;
extern fn ra8_decomp_budget_charge_entry(b: ?*Budget) callconv(.c) c_int;
extern fn ra8_decomp_budget_charge_iter(b: ?*Budget) callconv(.c) c_int;
extern fn ra8_decomp_check_declared(limits: ?*const Limits, comp_size: u64, out_size: u64) callconv(.c) c_int;

// ---- The public types (inc/unarch_tar.h, inc/unarch_io.h) -----------------

/// unarch_read_fn: the positioned byte reader the archive backs onto.
pub const unarch_read_fn = *const fn (ctx: ?*anyopaque, offset: u64, buf: ?*anyopaque, len: usize) callconv(.c) usize;

/// unarch_tar_t: one open archive plus the walk's running budget.
pub const unarch_tar_t = extern struct {
    read: ?unarch_read_fn = null,
    ctx: ?*anyopaque = null,
    size: u64 = 0,
    budget: Budget = .{},
    live: bool = false,
};

/// unarch_tar_entry_t: one decoded member descriptor.
pub const unarch_tar_entry_t = extern struct {
    data_off: u64 = 0,
    size: u64 = 0,
    next_off: u64 = 0,
    name_len: u16 = 0,
    is_file: u8 = 0,
    is_dir: u8 = 0,
};

/// tar_meta_t: overrides accumulated over one member's pax / GNU prelude.
const Meta = struct {
    name_buf: ?[*]u8 = null,
    name_cap: u16 = 0,
    name_len: u16 = 0,
    have_path: bool = false,
    size_ovr: u64 = 0,
    have_size: bool = false,
};

/// Module-scope scratch for one meta block's data area: a function-local
/// static in the C TU. The single-threaded walker overwrites it per block.
var s_tar_meta_scratch: [k_unarch_tar_pax_max]u8 = undefined;

// ---- Walk internals -------------------------------------------------------

/// Read one full header block at `off`, bounds-checked: a block that would
/// overrun the archive or a short read from the backing rejects the member.
fn internal_read_block(t: *unarch_tar_t, off: u64, block: *[k_unarch_tar_block]u8) u16 {
    if (t.size < k_unarch_tar_block or off > t.size - k_unarch_tar_block) {
        return k_ra8_err_validation_failed; // partial trailing block
    }
    const got = t.read.?(t.ctx, off, block, k_unarch_tar_block);
    if (got != k_unarch_tar_block) {
        return k_ra8_err_validation_failed; // backing truncated
    }
    return k_ra8_ok;
}

/// Compute the offset of the block after a member, overflow-checked:
/// `next = off + block + ceil(dsize / block) * block`. Any step that would
/// wrap u64 rejects the member; the walk strictly advances or dies.
fn internal_next_off(off: u64, dsize: u64, next: *u64) u16 {
    const blk: u64 = k_unarch_tar_block;
    if (dsize > std.math.maxInt(u64) - (blk - 1)) {
        return k_ra8_err_validation_failed; // rounding would wrap
    }
    const blocks = (dsize + (blk - 1)) / blk;
    if (blocks > (std.math.maxInt(u64) / blk) - 1) {
        return k_ra8_err_validation_failed; // span would wrap
    }
    const span = (blocks + 1) * blk;
    if (off > std.math.maxInt(u64) - span) {
        return k_ra8_err_validation_failed; // next offset would wrap
    }
    next.* = off + span;
    return k_ra8_ok;
}

/// Consume one pax / GNU-longname meta block's data area: bounds it to
/// k_unarch_tar_pax_max, reads it whole, and applies it. A pax header
/// contributes path / size overrides; a longname contributes the
/// NUL-stripped name. Skipped meta kinds ('g', 'K') never arrive here.
fn internal_meta_consume(t: *unarch_tar_t, type_tag: u8, doff: u64, dsize: u64, meta: *Meta) u16 {
    if (dsize > k_unarch_tar_pax_max) {
        return k_ra8_err_validation_failed; // meta flood: reject the member
    }
    const dlen: usize = @intCast(dsize);
    const got = t.read.?(t.ctx, doff, &s_tar_meta_scratch, dlen);
    if (got != dlen) {
        return k_ra8_err_validation_failed; // backing truncated
    }
    if (type_tag == fields.k_ra8_tar_type_pax) {
        return fields.priv_unarch_tar_pax_parse(
            &s_tar_meta_scratch,
            dlen,
            meta.name_buf,
            meta.name_cap,
            &meta.name_len,
            &meta.have_path,
            &meta.size_ovr,
            &meta.have_size,
        );
    }
    // GNU longname: the data is the member name, usually NUL-padded.
    var nlen = dlen;
    while (nlen > 0) : (nlen -= 1) {
        if (s_tar_meta_scratch[nlen - 1] != 0) {
            break;
        }
    }
    const want = @min(nlen, @as(usize, meta.name_cap));
    if (want > 0) {
        @memcpy(meta.name_buf.?[0..want], s_tar_meta_scratch[0..want]);
    }
    meta.name_len = @intCast(want);
    meta.have_path = true;
    return k_ra8_ok;
}

/// Measure one NUL-terminated-or-full ustar header field: scans no farther
/// than `capacity` bytes from `offset`. Pure, no header byte is modified.
fn internal_field_length(block: *const [k_unarch_tar_block]u8, offset: usize, capacity: usize) usize {
    var length: usize = 0;
    while (length < capacity and block[offset + length] != 0) {
        length += 1;
    }
    return length;
}

/// Assemble a ustar member name (prefix + '/' + name), clamped. Used when
/// no pax path or longname override exists; a non-empty prefix contributes
/// `prefix '/'` before the name field.
fn internal_ustar_name(block: *const [k_unarch_tar_block]u8, meta: *Meta) void {
    const n_len = internal_field_length(block, fields.k_ra8_tar_off_name, fields.k_ra8_tar_len_name);
    const p_len = internal_field_length(block, fields.k_ra8_tar_off_prefix, fields.k_ra8_tar_len_prefix);
    var used: usize = 0;
    if (p_len > 0) {
        const pw = @min(p_len, @as(usize, meta.name_cap));
        if (pw > 0) {
            @memcpy(meta.name_buf.?[0..pw], block[fields.k_ra8_tar_off_prefix..][0..pw]);
        }
        used = pw;
        if (used < meta.name_cap) {
            meta.name_buf.?[used] = '/';
            used += 1;
        }
    }
    const room = @as(usize, meta.name_cap) - used;
    const nw = @min(n_len, room);
    if (nw > 0) {
        @memcpy(meta.name_buf.?[used..][0..nw], block[fields.k_ra8_tar_off_name..][0..nw]);
    }
    meta.name_len = @intCast(used + nw);
}

/// Validate one header block and decode its size / type / next offset:
/// checksum, magic, numeric size field, and the overflow-checked advance.
/// Shared by the meta prelude and the real member.
fn internal_decode_header(
    block: *[k_unarch_tar_block]u8,
    off: u64,
    type_tag: *u8,
    dsize: *u64,
    next: *u64,
) u16 {
    if (!fields.priv_unarch_tar_checksum_ok(block)) {
        return k_ra8_err_validation_failed;
    }
    if (!fields.priv_unarch_tar_magic_ok(block)) {
        return k_ra8_err_validation_failed;
    }
    const serr = fields.priv_unarch_tar_num(block[fields.k_ra8_tar_off_size..], fields.k_ra8_tar_len_size, dsize);
    if (serr != k_ra8_ok) {
        return serr;
    }
    type_tag.* = fields.priv_unarch_tar_classify(block[fields.k_ra8_tar_off_type]);
    return internal_next_off(off, dsize.*, next);
}

/// Finish decoding the real member header into the caller entry: applies
/// the accumulated overrides (pax size wins over the header field and
/// re-derives the data span; pax path / longname wins over the ustar name
/// fields), bounds the data area against the archive and the policy's
/// output cap, and charges the entry budget.
fn internal_finish_member(
    t: *unarch_tar_t,
    block: *[k_unarch_tar_block]u8,
    off: u64,
    type_tag: u8,
    dsize: u64,
    meta: *Meta,
    out: *unarch_tar_entry_t,
) u16 {
    var msize = dsize;
    if (meta.have_size) {
        msize = meta.size_ovr; // pax size record overrides the header field
    }
    var next: u64 = 0;
    const aerr = internal_next_off(off, msize, &next);
    if (aerr != k_ra8_ok) {
        return aerr;
    }
    const doff = off + k_unarch_tar_block;
    if (msize > t.size - doff) {
        return k_ra8_err_validation_failed; // lying size: data overruns archive
    }
    const derr = ra8_decomp_check_declared(&t.budget.limits, msize, msize);
    if (derr != 0) {
        return @intCast(derr);
    }
    const eerr = ra8_decomp_budget_charge_entry(&t.budget);
    if (eerr != 0) {
        return @intCast(eerr);
    }
    if (!meta.have_path) {
        internal_ustar_name(block, meta);
    }
    out.data_off = doff;
    out.size = msize;
    out.next_off = next;
    out.name_len = meta.name_len;
    out.is_file = if (type_tag == fields.k_ra8_tar_type_file) 1 else 0;
    out.is_dir = if (type_tag == fields.k_ra8_tar_type_dir) 1 else 0;
    return k_ra8_ok;
}

// ---- The public ABI (inc/unarch_tar.h) ------------------------------------

/// unarch_tar_probe: whether `len` bytes at `block` look like a tar header.
pub export fn unarch_tar_probe(block: ?[*]const u8, len: usize) callconv(.c) bool {
    if (block == null) {
        return false;
    }
    if (len < k_unarch_tar_block) {
        return false;
    }
    if (!fields.priv_unarch_tar_magic_ok(block)) {
        return false;
    }
    return fields.priv_unarch_tar_checksum_ok(block);
}

/// unarch_tar_open: bind a backing to a walker and probe its first block.
/// The policy is validated and the walk's budget initialised here; the
/// budget then accumulates across every unarch_tar_next call.
pub export fn unarch_tar_open(
    t: ?*unarch_tar_t,
    read: ?unarch_read_fn,
    ctx: ?*anyopaque,
    size: u64,
    limits: ?*const Limits,
) callconv(.c) u16 {
    if (t == null) {
        return k_ra8_err_null_ptr;
    }
    t.?.* = .{};
    if (read == null) {
        return k_ra8_err_null_ptr;
    }
    if (size < k_unarch_tar_block) {
        return k_ra8_err_invalid_size;
    }
    const berr = ra8_decomp_budget_init(&t.?.budget, limits);
    if (berr != 0) {
        return @intCast(berr);
    }
    var block = std.mem.zeroes([k_unarch_tar_block]u8);
    const got = read.?(ctx, 0, &block, k_unarch_tar_block);
    if (got != k_unarch_tar_block) {
        return k_ra8_err_invalid_size;
    }
    if (!unarch_tar_probe(&block, k_unarch_tar_block)) {
        return k_ra8_err_not_supported;
    }
    t.?.read = read;
    t.?.ctx = ctx;
    t.?.size = size;
    t.?.live = true;
    return k_ra8_ok;
}

/// unarch_tar_next: decode the member at `off` and advance. Consumes the
/// member's bounded pax / GNU meta prelude and its real header, charging
/// the walker's embedded budget one iteration per examined block and one
/// entry per returned member, so the whole enumeration is policy-bounded.
/// All offset arithmetic is overflow-checked: a hostile size field cannot
/// wrap the walk backwards.
pub export fn unarch_tar_next(
    t: ?*unarch_tar_t,
    off: u64,
    name_buf: ?[*]u8,
    name_cap: u16,
    out: ?*unarch_tar_entry_t,
) callconv(.c) u16 {
    if (t == null or out == null) {
        return k_ra8_err_null_ptr;
    }
    out.?.* = .{};
    const walker = t.?;
    if (!walker.live) {
        return k_ra8_err_invalid_state;
    }
    if (name_cap > 0 and name_buf == null) {
        return k_ra8_err_null_ptr;
    }
    var meta = Meta{ .name_buf = name_buf, .name_cap = name_cap };
    var cur = off;
    var m: u32 = 0;
    while (m <= k_unarch_tar_meta_max) : (m += 1) {
        const ierr = ra8_decomp_budget_charge_iter(&walker.budget);
        if (ierr != 0) {
            return @intCast(ierr);
        }
        if (cur >= walker.size) {
            return k_ra8_err_not_found; // clean EOF without an end marker
        }
        var block = std.mem.zeroes([k_unarch_tar_block]u8);
        const rerr = internal_read_block(walker, cur, &block);
        if (rerr != k_ra8_ok) {
            return rerr;
        }
        if (fields.priv_unarch_tar_block_zero(&block)) {
            return k_ra8_err_not_found; // end-of-archive marker
        }
        var type_tag: u8 = fields.k_ra8_tar_type_other;
        var dsize: u64 = 0;
        var next: u64 = 0;
        const herr = internal_decode_header(&block, cur, &type_tag, &dsize, &next);
        if (herr != k_ra8_ok) {
            return herr;
        }
        if (type_tag == fields.k_ra8_tar_type_pax or type_tag == fields.k_ra8_tar_type_longname) {
            const doff = cur + k_unarch_tar_block;
            if (dsize > walker.size - doff) {
                return k_ra8_err_validation_failed; // meta data overruns archive
            }
            const merr = internal_meta_consume(walker, type_tag, doff, dsize, &meta);
            if (merr != k_ra8_ok) {
                return merr;
            }
            cur = next;
            continue;
        }
        if (type_tag == fields.k_ra8_tar_type_meta) {
            cur = next; // skipped meta ('g' global, 'K' longlink)
            continue;
        }
        const ferr = internal_finish_member(walker, &block, cur, type_tag, dsize, &meta, out.?);
        if (ferr != k_ra8_ok) {
            out.?.* = .{};
            return ferr;
        }
        return k_ra8_ok;
    }
    return k_ra8_err_validation_failed; // meta prelude longer than the cap
}

/// unarch_tar_read: fetch one member's whole data area. A forged entry
/// (data area outside the archive), a too-small buffer, or a truncated
/// backing is rejected fail-closed; an empty member reads as ok with zero
/// bytes.
pub export fn unarch_tar_read(
    t: ?*const unarch_tar_t,
    ent: ?*const unarch_tar_entry_t,
    buf: ?*anyopaque,
    cap: usize,
    got: ?*usize,
) callconv(.c) u16 {
    if (t == null or ent == null or buf == null or got == null) {
        return k_ra8_err_null_ptr;
    }
    got.?.* = 0;
    const walker = t.?;
    const entry = ent.?;
    if (!walker.live) {
        return k_ra8_err_invalid_state;
    }
    if (entry.is_file == 0) {
        return k_ra8_err_not_supported;
    }
    if (@as(u64, cap) < entry.size) {
        return k_ra8_err_no_mem;
    }
    if (entry.data_off > walker.size) {
        return k_ra8_err_invalid_size; // forged entry: data area outside archive
    }
    if (entry.size > walker.size - entry.data_off) {
        return k_ra8_err_invalid_size; // forged entry: data area overruns archive
    }
    if (entry.size == 0) {
        return k_ra8_ok; // empty member: nothing to fetch
    }
    const want: usize = @intCast(entry.size);
    const n = walker.read.?(walker.ctx, entry.data_off, buf, want);
    if (@as(u64, n) != entry.size) {
        return k_ra8_err_invalid_size; // backing truncated
    }
    got.?.* = n;
    return k_ra8_ok;
}
