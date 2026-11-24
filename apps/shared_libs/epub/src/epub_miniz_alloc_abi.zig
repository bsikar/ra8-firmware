//! Caller-owned bounded-arena allocator for miniz, ported from
//! src/epub_miniz_alloc.c (RA8FW-1167). Semantics and error paths match the
//! C TU exactly: first-fit over one caller-owned workspace, in-band block
//! headers, free coalescing on every free, realloc preserving the original
//! block when a grow cannot be satisfied. No global arena and no host heap
//! fallback exist. The inc/ header stays the contract for the C side.

const std = @import("std");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, enum : uint16_t) --------------
const ra8_err_t = u16;
pub const k_ra8_ok: ra8_err_t = 0;
pub const k_ra8_err_invalid_arg: ra8_err_t = 0x103;
pub const k_ra8_err_invalid_size: ra8_err_t = 0x105;
pub const k_ra8_err_null_ptr: ra8_err_t = 0x504;

// ---- arena geometry (epub_miniz_alloc.h) -----------------------------------
pub const k_epub_miniz_pool_bytes = 163840;

/// alignof(max_align_t): 16 on the 64-bit SysV hosts, 8 under AAPCS32.
pub const k_priv_align = if (@sizeOf(usize) == 8) 16 else 8;

/// alignof(max_align_t)-aligned storage for one miniz arena.
const epub_miniz_workspace_t = extern struct {
    bytes: [k_epub_miniz_pool_bytes]u8 align(k_priv_align),
};

pub const epub_miniz_arena_t = extern struct {
    base: ?[*]u8,
    capacity: usize,
    initialized: u8,
};

/// Header prefixing every arena block.
const priv_blk_t = extern struct {
    size: usize,
    is_free: usize,
};

pub const k_priv_hdr_bytes = ((@sizeOf(priv_blk_t) + k_priv_align - 1) / k_priv_align) * k_priv_align;
pub const k_priv_blk_free: usize = 1;
pub const k_priv_blk_used: usize = 0;
const k_priv_walk_max = k_epub_miniz_pool_bytes / (k_priv_hdr_bytes + k_priv_align);

comptime {
    if ((k_epub_miniz_pool_bytes % k_priv_align) != 0)
        @compileError("miniz workspace size must be a whole number of aligned cells");
    if (@sizeOf(epub_miniz_workspace_t) != k_epub_miniz_pool_bytes)
        @compileError("public miniz workspace must have exact advertised capacity");
}

// ---- private helpers -------------------------------------------------------

/// Copy a pointer representation into an integer address (the C TU's bytewise
/// memcpy conversion; identical on the flat memory models we target).
fn internal_pointer_address(pointer: ?*const anyopaque) usize {
    const p = pointer orelse return 0;
    return @intFromPtr(p);
}

/// Validate an arena descriptor before using caller-owned storage: lifecycle,
/// exact capacity, backing pointer, and alignment. Pure, touches nothing.
pub fn internal_ready(arena: ?*const epub_miniz_arena_t) bool {
    const a = arena orelse return false;
    if (a.initialized != 1) return false;
    const base = a.base orelse return false;
    if (a.capacity != k_epub_miniz_pool_bytes) return false;
    return (@intFromPtr(base) % k_priv_align) == 0;
}

/// Cast a byte offset to an aligned block header.
pub fn internal_cell_at(arena: *const epub_miniz_arena_t, off: usize) *priv_blk_t {
    const base = @intFromPtr(arena.base.?);
    return @ptrFromInt(base + off);
}

/// The arena's one-past byte address.
fn internal_end(arena: *const epub_miniz_arena_t) usize {
    return @intFromPtr(arena.base.?) + arena.capacity;
}

/// Address of the payload immediately following the block header.
fn internal_payload(block: *const priv_blk_t) usize {
    return @intFromPtr(block) + k_priv_hdr_bytes;
}

/// Header for an allocation payload address.
pub fn internal_header(arena: *const epub_miniz_arena_t, address: *anyopaque) *priv_blk_t {
    _ = arena;
    const payload = @intFromPtr(address);
    return @ptrFromInt(payload - k_priv_hdr_bytes);
}

/// Block immediately following the given block.
fn internal_next(arena: *const epub_miniz_arena_t, block: *const priv_blk_t) *priv_blk_t {
    _ = arena;
    const end = internal_payload(block) + block.size;
    return @ptrFromInt(end);
}

/// Whether an address lies in the payload-bearing arena range. Rejects
/// inactive arenas, excludes the header-only prefix and the one-past byte,
/// and never dereferences the candidate address.
pub fn internal_in_pool(arena: ?*const epub_miniz_arena_t, address: ?*const anyopaque) bool {
    if (!internal_ready(arena)) return false;
    const a = arena.?;
    const value = @intFromPtr(address.?);
    const base = @intFromPtr(a.base.?);
    const first = base + k_priv_hdr_bytes;
    const end = base + a.capacity;
    return (value >= first) and (value < end);
}

/// Align a pool-bounded request upward to a complete arena cell.
fn internal_align_up(bytes: usize) usize {
    const mask: usize = k_priv_align - 1;
    return (bytes + mask) & ~mask;
}

/// Validate and multiply a miniz count/size request exactly once.
fn internal_request_bytes(items: usize, size: usize, out_bytes: *usize) bool {
    if ((size != 0) and (items > (std.math.maxInt(usize) / size))) return false;
    const bytes = items * size;
    if (bytes > k_epub_miniz_pool_bytes) return false;
    out_bytes.* = bytes;
    return true;
}

/// Split a block when its remainder can hold another allocation; unusably
/// small tails stay inside the live block.
fn internal_split(arena: *const epub_miniz_arena_t, block: *priv_blk_t, need: usize) void {
    _ = arena;
    const remainder_min = k_priv_hdr_bytes + k_priv_align;
    if ((block.size - need) < remainder_min) return;
    const rem_addr = internal_payload(block) + need;
    const remainder: *priv_blk_t = @ptrFromInt(rem_addr);
    remainder.size = block.size - need - k_priv_hdr_bytes;
    remainder.is_free = k_priv_blk_free;
    block.size = need;
}

/// Merge all adjacent free blocks in the arena without moving live payload
/// bytes. The fixed walk guard bounds behavior if metadata is corrupted.
fn internal_coalesce(arena: *const epub_miniz_arena_t) void {
    const end = internal_end(arena);
    var block = internal_cell_at(arena, 0);
    var guard: usize = 0;
    while (guard < k_priv_walk_max) : (guard += 1) {
        if (@intFromPtr(block) >= end) break;
        if (block.is_free == k_priv_blk_free) {
            var next = internal_next(arena, block);
            while ((@intFromPtr(next) < end) and (next.is_free == k_priv_blk_free)) {
                block.size += k_priv_hdr_bytes + next.size;
                next = internal_next(arena, block);
            }
        }
        block = internal_next(arena, block);
    }
}

// ---- public API (epub_miniz_alloc.h) ---------------------------------------

/// Initialise or reset a miniz arena over caller-owned storage.
pub export fn epub_miniz_arena_init(arena: ?*epub_miniz_arena_t, workspace: ?*anyopaque, workspace_bytes: usize) callconv(.c) ra8_err_t {
    if ((arena == null) or (workspace == null)) return k_ra8_err_null_ptr;
    if (workspace_bytes < k_epub_miniz_pool_bytes) return k_ra8_err_invalid_size;
    if ((@intFromPtr(workspace.?) % k_priv_align) != 0) return k_ra8_err_invalid_arg;
    const next = epub_miniz_arena_t{
        .base = @ptrCast(workspace.?),
        .capacity = k_epub_miniz_pool_bytes,
        .initialized = 1,
    };
    // The head block lives at offset 0 of the caller's workspace, which is
    // exactly what internal_cell_at(&next, 0) computes.
    const head: *priv_blk_t = @ptrFromInt(@intFromPtr(workspace.?));
    head.size = next.capacity - k_priv_hdr_bytes;
    head.is_free = k_priv_blk_free;
    arena.?.* = next;
    return k_ra8_ok;
}

/// Invalidate an arena descriptor after miniz has released its blocks.
pub export fn epub_miniz_arena_deinit(arena: ?*epub_miniz_arena_t) callconv(.c) void {
    if (arena) |a| {
        a.* = std.mem.zeroes(epub_miniz_arena_t);
    }
}

/// miniz-compatible allocator (mz_alloc_func): first-fit over the free list,
/// max_align_t aligned, NULL on overflow of items*size or pool exhaustion.
pub export fn epub_miniz_alloc(opaque_: ?*anyopaque, items: usize, size: usize) callconv(.c) ?*anyopaque {
    const arena: ?*epub_miniz_arena_t = @ptrCast(@alignCast(opaque_));
    if (!internal_ready(arena)) return null;
    const a = arena.?;
    var bytes: usize = 0;
    if (!internal_request_bytes(items, size, &bytes)) return null;
    var need = internal_align_up(bytes);
    if (need == 0) need = k_priv_align;
    const end = internal_end(a);
    var block = internal_cell_at(a, 0);
    var guard: usize = 0;
    while (guard < k_priv_walk_max) : (guard += 1) {
        if (@intFromPtr(block) >= end) break;
        if ((block.is_free == k_priv_blk_free) and (block.size >= need)) {
            internal_split(a, block, need);
            block.is_free = k_priv_blk_used;
            return @ptrFromInt(internal_payload(block));
        }
        block = internal_next(a, block);
    }
    return null;
}

/// miniz-compatible free (mz_free_func): mark the block free and coalesce.
/// A NULL address, invalid arena, or pointer outside that arena is ignored.
pub export fn epub_miniz_free(opaque_: ?*anyopaque, address: ?*anyopaque) callconv(.c) void {
    const arena: ?*epub_miniz_arena_t = @ptrCast(@alignCast(opaque_));
    if ((address == null) or !internal_in_pool(arena, address)) return;
    const block = internal_header(arena.?, address.?);
    block.is_free = k_priv_blk_free;
    internal_coalesce(arena.?);
}

/// miniz-compatible realloc (mz_realloc_func): alloc on NULL, free and NULL on
/// a zero-byte request, in place when the block already fits, otherwise move
/// the original payload bytes to the fresh block and free the old one.
pub export fn epub_miniz_realloc(opaque_: ?*anyopaque, address: ?*anyopaque, items: usize, size: usize) callconv(.c) ?*anyopaque {
    if (address == null) return epub_miniz_alloc(opaque_, items, size);
    const arena: ?*epub_miniz_arena_t = @ptrCast(@alignCast(opaque_));
    if (!internal_in_pool(arena, address)) return null;
    var bytes: usize = 0;
    if (!internal_request_bytes(items, size, &bytes)) return null;
    const need = internal_align_up(bytes);
    if (need == 0) {
        epub_miniz_free(opaque_, address);
        return null;
    }
    const block = internal_header(arena.?, address.?);
    if (block.size >= need) return address;
    const moved = epub_miniz_alloc(opaque_, items, size);
    if (moved == null) return null;
    const dst: [*]u8 = @ptrCast(moved.?);
    const src: [*]const u8 = @ptrCast(address.?);
    @memcpy(dst[0..block.size], src[0..block.size]);
    epub_miniz_free(opaque_, address);
    return moved;
}

// ---- tests (self-contained: the TU has no externs, so the tests drive a
// real workspace end to end) --------------------------------------------------
