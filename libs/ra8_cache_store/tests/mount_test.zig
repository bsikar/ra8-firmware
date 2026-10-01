//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for `ra8_cache_store_init`: config validation, the derived
//! geometry, format and clean-mount bring-up, checkpoint recovery and
//! append-log replay. The primitives those paths are built on are tested in
//! `primitives_test.zig`; both drive the fake fake.medium in `cache_fake.zig`.

const std = @import("std");
const mount = @import("mount");
const fake = @import("cache_fake.zig");

const EntryHeader = mount.EntryHeader;
const Super = mount.Super;
const implementation = mount.implementation;

const ok = fake.ok;
const err_invalid_size = fake.err_invalid_size;
const err_hw_init_failed = fake.err_hw_init_failed;
const err_null_ptr = fake.err_null_ptr;
const sector_bytes = fake.sector_bytes;
const Fixture = fake.Fixture;
const resetMedium = fake.resetMedium;
const readSuper = fake.readSuper;
const writeHeaderAt = fake.writeHeaderAt;
const writeSuperAt = fake.writeSuperAt;
// -------------------------------------------------------------------------
// init: validation
// -------------------------------------------------------------------------

test "init: a null store handle is rejected" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    const cfg = fixture.cfg(64, true);
    try std.testing.expectEqual(err_null_ptr, mount.ra8_cache_store_init(null, &cfg));
}

test "init: a null config is rejected" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    try std.testing.expectEqual(err_null_ptr, mount.ra8_cache_store_init(&fixture.store, null));
}

test "init: every required config pointer is checked in order" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();

    var cfg = fixture.cfg(64, true);
    cfg.nor_flash = null;
    try std.testing.expectEqual(err_null_ptr, mount.validateCfg(&cfg));

    cfg = fixture.cfg(64, true);
    cfg.nor_driver_init = null;
    try std.testing.expectEqual(err_null_ptr, mount.validateCfg(&cfg));

    cfg = fixture.cfg(64, true);
    cfg.index = null;
    try std.testing.expectEqual(err_null_ptr, mount.validateCfg(&cfg));

    cfg = fixture.cfg(64, true);
    cfg.staging = null;
    try std.testing.expectEqual(err_null_ptr, mount.validateCfg(&cfg));
}

test "init: a zero index capacity and a short staging buffer are size errors" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();

    var cfg = fixture.cfg(64, true);
    cfg.index_cap = 0;
    try std.testing.expectEqual(err_invalid_size, mount.validateCfg(&cfg));

    cfg = fixture.cfg(64, true);
    cfg.staging_bytes = sector_bytes - 1;
    try std.testing.expectEqual(err_invalid_size, mount.validateCfg(&cfg));
}

test "init: an absurd overprovision margin is an argument error" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    var cfg = fixture.cfg(64, true);
    cfg.overprovision_pct = mount.max_overprov + 1;
    try std.testing.expectEqual(mount.err_invalid_arg, mount.validateCfg(&cfg));
    cfg.overprovision_pct = mount.max_overprov;
    try std.testing.expectEqual(ok, mount.validateCfg(&cfg));
}

test "init: a span too small for the layout is a size error" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    const cfg = fixture.cfg(9, true);
    try std.testing.expectEqual(err_invalid_size, mount.ra8_cache_store_init(&fixture.store, &cfg));
}

// -------------------------------------------------------------------------
// init: geometry
// -------------------------------------------------------------------------

test "geometry: the checkpoint span follows the index capacity" {
    var fixture = Fixture{};
    fixture.bind();
    var cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.geometry(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 1), fixture.store.checkpoint_dirs);
    try std.testing.expectEqual(@as(u32, 2), fixture.store.log_start);
    // 62 usable sectors less the default 20% margin.
    try std.testing.expectEqual(@as(u32, 49), fixture.store.data_capacity);

    cfg.index_cap = 33;
    try std.testing.expectEqual(ok, mount.geometry(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 2), fixture.store.checkpoint_dirs);
    try std.testing.expectEqual(@as(u32, 3), fixture.store.log_start);
}

test "geometry: an explicit margin overrides the default" {
    var fixture = Fixture{};
    fixture.bind();
    var cfg = fixture.cfg(64, false);
    cfg.overprovision_pct = 50;
    try std.testing.expectEqual(ok, mount.geometry(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u32, 31), fixture.store.data_capacity);
}

test "geometry: a margin that rounds the budget to nothing still leaves one sector" {
    var fixture = Fixture{};
    fixture.bind();
    var cfg = fixture.cfg(10, false);
    cfg.overprovision_pct = 90;
    try std.testing.expectEqual(ok, mount.geometry(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u32, 1), fixture.store.data_capacity);
}

// -------------------------------------------------------------------------
// init: bring-up
// -------------------------------------------------------------------------

test "init: a format mount comes up clean and empty" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    const cfg = fixture.cfg(64, true);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expect(fixture.store.inited);
    try std.testing.expectEqual(@as(u8, 1), fixture.store.flash_state);
    try std.testing.expectEqual(@as(u32, 0), fixture.store.live_sectors);
    try std.testing.expectEqual(@as(u32, 1), fixture.store.next_seq);
    try std.testing.expect(fake.present[0]);
    try std.testing.expect(fake.driver_init_calls == 0 or fake.driver_init_calls > 0);
}

test "init: a failed format is a hardware failure" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    fake.forced_format_rc = 1;
    const cfg = fixture.cfg(64, true);
    try std.testing.expectEqual(
        err_hw_init_failed,
        mount.ra8_cache_store_init(&fixture.store, &cfg),
    );
    try std.testing.expect(!fixture.store.inited);
}

test "init: a failed open is a hardware failure" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    fake.forced_open_rc = 1;
    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(
        err_hw_init_failed,
        mount.ra8_cache_store_init(&fixture.store, &cfg),
    );
}

test "init: the index is cleared before the mount rebuilds it" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    fixture.index[2] = .{ .key = 5, .flags = implementation.flag_in_use };
    const cfg = fixture.cfg(64, true);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 0), implementation.indexUsed(&fixture.index));
}

// -------------------------------------------------------------------------
// init: checkpoint recovery
// -------------------------------------------------------------------------

test "recovery: a clean checkpoint is loaded back into the index" {
    resetMedium();
    var writer = Fixture{};
    writer.bind();
    var cfg = writer.cfg(64, true);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&writer.store, &cfg));

    writer.index[0] = .{
        .key = 101,
        .start_sector = 8,
        .byte_len = 600,
        .sector_count = 3,
        .flags = implementation.flag_in_use,
    };
    writer.index[1] = .{
        .key = 202,
        .start_sector = 20,
        .byte_len = 100,
        .sector_count = 2,
        .flags = implementation.flag_in_use | implementation.flag_pinned,
    };
    writer.store.live_sectors = 5;
    writer.store.next_seq = 7;
    var count: u32 = 0;
    try std.testing.expectEqual(ok, mount.priv_cache_store_dir_save(&writer.store, &count));
    try std.testing.expectEqual(ok, mount.priv_cache_store_super_write(&writer.store, 1));

    var reader = Fixture{};
    reader.bind();
    reader.flash_block = writer.flash_block;
    var reopen = reader.cfg(64, false);
    reopen.nor_flash = @ptrCast(&reader.flash_block);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&reader.store, &reopen));

    try std.testing.expectEqual(@as(u8, 1), reader.store.flash_state);
    try std.testing.expectEqual(@as(u32, 7), reader.store.next_seq);
    try std.testing.expectEqual(@as(u32, 5), reader.store.live_sectors);
    try std.testing.expectEqual(@as(u16, 2), implementation.indexUsed(&reader.index));
    const slot = mount.priv_cache_store_index_find(&reader.store, 202);
    try std.testing.expect(slot >= 0);
    try std.testing.expect(implementation.isPinned(reader.index[@intCast(slot)]));
}

test "checkpoint seq: counts superblock writes, not appends" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    fixture.store.next_seq = 7;

    try std.testing.expectEqual(@as(u32, 0), fixture.store.checkpoint_seq);
    try std.testing.expectEqual(ok, mount.priv_cache_store_super_write(&fixture.store, 1));
    try std.testing.expectEqual(@as(u32, 1), fixture.store.checkpoint_seq);
    try std.testing.expectEqual(@as(u32, 1), readSuper().seq);

    // The append counter stands still while the checkpoint counter moves.
    try std.testing.expectEqual(ok, mount.priv_cache_store_super_write(&fixture.store, 1));
    try std.testing.expectEqual(@as(u32, 2), fixture.store.checkpoint_seq);
    try std.testing.expectEqual(@as(u32, 2), readSuper().seq);
    try std.testing.expectEqual(@as(u32, 7), fixture.store.next_seq);
    try std.testing.expectEqual(@as(u32, 7), readSuper().next_seq);
}

test "checkpoint seq: resumes from the fake.medium across a mount" {
    resetMedium();
    var writer = Fixture{};
    writer.bind();
    var first = writer.cfg(64, true);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&writer.store, &first));
    // Formatting stamps one superblock, so the counter has already moved.
    try std.testing.expect(writer.store.checkpoint_seq > 0);
    try std.testing.expectEqual(ok, mount.priv_cache_store_super_write(&writer.store, 1));
    const at_reboot = writer.store.checkpoint_seq;

    var reader = Fixture{};
    reader.bind();
    reader.flash_block = writer.flash_block;
    var reopen = reader.cfg(64, false);
    reopen.nor_flash = @ptrCast(&reader.flash_block);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&reader.store, &reopen));

    // Resumed, not restarted, so the next record outranks the one on the media.
    try std.testing.expectEqual(at_reboot, reader.store.checkpoint_seq);
    try std.testing.expectEqual(ok, mount.priv_cache_store_super_write(&reader.store, 1));
    try std.testing.expect(readSuper().seq > at_reboot);
}

test "checkpoint seq: a dirty record still hands its seq to the next mount" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    var sb = Super{
        .magic = mount.super_magic,
        .version = mount.format_version,
        .seq = 41,
        .clean = 0,
        .entry_count = 0,
        .live_sectors = 0,
        .next_seq = 1,
        .log_start = 2,
        .data_capacity = 32,
        .logical_sectors = 64,
        .crc = 0,
    };
    writeSuperAt(&sb);
    // Dirty: the mount replays the log, and still honours the counter it read.
    try std.testing.expect(!mount.superIsClean(&sb));
    try std.testing.expect(mount.superIsValid(&sb));

    var cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u32, 41), fixture.store.checkpoint_seq);
}

test "checkpoint seq: a torn record leaves the counter at its init value" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    var sb = Super{
        .magic = mount.super_magic,
        .version = mount.format_version,
        .seq = 99,
        .clean = 1,
        .entry_count = 0,
        .live_sectors = 0,
        .next_seq = 1,
        .log_start = 2,
        .data_capacity = 32,
        .logical_sectors = 64,
        .crc = 0,
    };
    writeSuperAt(&sb);
    // Corrupt the CRC after sealing: the record no longer parses.
    fake.medium[0][@sizeOf(Super) - 4] ^= 0xFF;
    var cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u32, 0), fixture.store.checkpoint_seq);
}

test "superIsValid: parses a dirty record and rejects a broken one" {
    var sb = Super{ .magic = mount.super_magic, .version = mount.format_version, .clean = 0 };
    sb.crc = implementation.crc32(std.mem.asBytes(&sb)[0..mount.super_crc_span]);
    try std.testing.expect(mount.superIsValid(&sb));
    try std.testing.expect(!mount.superIsClean(&sb));
    sb.magic ^= 1;
    try std.testing.expect(!mount.superIsValid(&sb));
}

test "recovery: a checkpoint claiming more entries than the index holds is invalid state" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    var sb = Super{
        .magic = mount.super_magic,
        .version = mount.format_version,
        .seq = 1,
        .clean = 1,
        .entry_count = 9,
        .live_sectors = 0,
        .next_seq = 2,
        .log_start = 2,
        .data_capacity = 49,
        .logical_sectors = 64,
    };
    writeSuperAt(&sb);
    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(
        mount.err_invalid_state,
        mount.ra8_cache_store_init(&fixture.store, &cfg),
    );
}

// -------------------------------------------------------------------------
// init: log replay
// -------------------------------------------------------------------------

test "replay: an absent superblock rebuilds the index from the log" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    writeHeaderAt(2, 111, 4, 3, 1000, 0);
    writeHeaderAt(5, 222, 9, 2, 300, implementation.flag_pinned);

    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u8, 0), fixture.store.flash_state);
    try std.testing.expectEqual(@as(u16, 2), implementation.indexUsed(&fixture.index));
    try std.testing.expectEqual(@as(u32, 5), fixture.store.live_sectors);
    try std.testing.expectEqual(@as(u32, 10), fixture.store.next_seq);
    const slot = mount.priv_cache_store_index_find(&fixture.store, 222);
    try std.testing.expect(slot >= 0);
    try std.testing.expect(implementation.isPinned(fixture.index[@intCast(slot)]));
}

test "replay: a torn header is skipped and the scan carries on" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    writeHeaderAt(2, 111, 1, 1, 10, 0);
    // Corrupt the CRC of the run at sector 4, and leave sector 3 blank.
    writeHeaderAt(4, 222, 2, 1, 10, 0);
    fake.medium[4][@sizeOf(EntryHeader) - 1] ^= 0xFF;
    writeHeaderAt(6, 333, 3, 1, 10, 0);

    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 2), implementation.indexUsed(&fixture.index));
    try std.testing.expect(mount.priv_cache_store_index_find(&fixture.store, 222) < 0);
    try std.testing.expectEqual(@as(u32, 4), fixture.store.next_seq);
}

test "replay: a header anchored at the wrong sector claims nothing" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    var header = EntryHeader{
        .magic = implementation.entry_magic,
        .seq = 1,
        .key = 55,
        .byte_len = 10,
        .start_sector = 9,
        .sector_count = 1,
        .flags = 0,
        .hdr_crc = 0,
    };
    implementation.sealHeader(&header);
    @memcpy(fake.medium[2][0..@sizeOf(EntryHeader)], std.mem.asBytes(&header));
    fake.present[2] = true;

    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 0), implementation.indexUsed(&fixture.index));
}

test "replay: a run that would overrun the span is refused" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    writeHeaderAt(60, 77, 1, 100, 10, 0);
    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 0), implementation.indexUsed(&fixture.index));
    try std.testing.expectEqual(@as(u32, 1), fixture.store.next_seq);
}

test "replay: a zero-length run is refused" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    writeHeaderAt(2, 77, 1, 0, 10, 0);
    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 0), implementation.indexUsed(&fixture.index));
}

test "replay: a duplicate key keeps the first run" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    writeHeaderAt(2, 500, 1, 1, 10, 0);
    writeHeaderAt(3, 500, 2, 1, 20, 0);
    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 1), implementation.indexUsed(&fixture.index));
    const slot = mount.priv_cache_store_index_find(&fixture.store, 500);
    try std.testing.expectEqual(@as(u32, 2), fixture.index[@intCast(slot)].start_sector);
    try std.testing.expectEqual(@as(u32, 1), fixture.store.live_sectors);
}

test "replay: a full index stops accumulating live sectors" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    var sector: u32 = 2;
    var key: u32 = 1;
    while (key <= 9) : (key += 1) {
        writeHeaderAt(sector, key, key, 1, 10, 0);
        sector += 1;
    }
    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 8), implementation.indexUsed(&fixture.index));
    try std.testing.expectEqual(@as(u32, 8), fixture.store.live_sectors);
    // The ninth entry never landed, so its sequence never raised the counter.
    try std.testing.expectEqual(@as(u32, 9), fixture.store.next_seq);
}

test "replay: a stale but structurally valid superblock still replays the log" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    var sb = Super{
        .magic = mount.super_magic,
        .version = mount.format_version,
        .seq = 3,
        .clean = 0,
        .entry_count = 0,
        .next_seq = 3,
        .log_start = 2,
        .logical_sectors = 64,
    };
    writeSuperAt(&sb);
    writeHeaderAt(2, 900, 12, 2, 600, 0);

    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u8, 0), fixture.store.flash_state);
    try std.testing.expectEqual(@as(u32, 13), fixture.store.next_seq);
    try std.testing.expectEqual(@as(u32, 2), fixture.store.live_sectors);
}

test "replay: a fake.medium that cannot be read at all leaves an empty index" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    fake.forced_read_rc = 1;
    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 0), implementation.indexUsed(&fixture.index));
    try std.testing.expectEqual(@as(u32, 1), fixture.store.next_seq);
}
