//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! FAT volume geometry and partition selection: BPB sector-size validation,
//! mount geometry derivation, and MBR/GPT partition lookup. Ported out of
//! `ra8_fs_fat_mount.c` (RA8FW-1130); the C half consumes these through the
//! extern declarations at the top of that file. The remaining mount path
//! (BPB parse, boot-sector read, probe, format, mount/unmount) and the
//! sector arena follow before the C file itself is retired.

const std = @import("std");
const c = @import("fs_c.zig").c;

/// True when `bs` is a power of two inside the supported sector range.
///
/// Every BPB `bytes_per_sector` and every format option passes through here
/// before it is allowed to size a buffer, so the guard is the single place
/// the 512..4096 power-of-two rule lives.
export fn internal_bps_valid(bs: u32) callconv(.c) u8 {
    const valid = bs >= c.k_ra8_fs_sector_min and bs <= c.k_ra8_fs_sector_max and
        (bs & (bs - 1)) == 0;
    return @intFromBool(valid);
}

/// Derive region LBAs and the FAT type from an already-parsed BPB.
///
/// Walks the mount's BPB fields forward: the FAT region starts at the
/// reserved sectors, the root directory follows both FAT copies, and the
/// data region follows the fixed root (FAT12/16) or none (FAT32/exFAT).
/// The cluster count then selects the FAT type with the thresholds from
/// MS FAT spec sec 3.5. Returns `k_ra8_err_validation_failed` when the
/// medium ends before the data region begins.
export fn internal_compute_geometry(m: [*c]c.ra8_fs_mount_t) callconv(.c) c.ra8_err_t {
    const bps: u32 = m.*.bytes_per_sector;
    m.*.first_fat_lba = m.*.reserved_sectors;
    const root_dir_sectors: u32 =
        (m.*.root_entries * c.k_ra8_fs_dir_entry_bytes + bps - 1) / bps;
    m.*.first_root_lba = @as(u64, m.*.first_fat_lba) +
        @as(u64, m.*.num_fats) * m.*.fat_size_sectors;
    m.*.first_data_lba = m.*.first_root_lba + root_dir_sectors;
    if (m.*.total_sectors < m.*.first_data_lba) {
        return c.k_ra8_err_validation_failed;
    }
    const data_sectors: u64 = m.*.total_sectors - m.*.first_data_lba;
    m.*.count_of_clusters = @intCast(data_sectors / m.*.sectors_per_cluster);
    if (m.*.count_of_clusters < c.k_cluster_count_fat12_max) {
        m.*.type = c.k_ra8_fs_type_fat12;
    } else if (m.*.count_of_clusters < c.k_cluster_count_fat16_max) {
        m.*.type = c.k_ra8_fs_type_fat16;
    } else {
        m.*.type = c.k_ra8_fs_type_fat32;
    }
    return c.k_ra8_ok;
}

/// Return partition 0's first LBA from an MBR in `buf`, or 0.
///
/// Used only after the LBA-0 BPB parse fails: a standard SD card is
/// MBR-partitioned, so the FAT boot sector lives at partition 0's start
/// LBA, not at LBA 0. Returns 0 when the buffer is not a usable MBR (no
/// 0x55AA signature, or an unused partition 0), which leaves the caller's
/// original error in force.
export fn internal_mbr_part0_lba(buf: [*c]const u8) callconv(.c) u32 {
    if (buf[c.k_bpb_off_signature_lo] != c.k_bpb_sig_lo) {
        return 0;
    }
    if (buf[c.k_bpb_off_signature_hi] != c.k_bpb_sig_hi) {
        return 0;
    }
    if (buf[c.k_mbr_off_part0_type] == 0) {
        return 0;
    }
    return std.mem.readInt(u32, buf[c.k_mbr_off_part0_lba..][0..4], .little);
}

/// Return the first LBA of MBR primary entry `index`.
///
/// Walks the four 16-byte primary entries, reading the type byte and the
/// first LBA of entry `index` at their base offsets plus `index` times the
/// entry stride. Unlike the auto path, an out-of-range index, an empty
/// slot, and a zero first LBA are reported distinctly.
export fn internal_mbr_select_entry(buf: [*c]const u8, index: u8, out_base: [*c]u64) callconv(.c) c.ra8_err_t {
    if (index >= c.k_mbr_part_entry_count) {
        return c.k_ra8_err_out_of_range;
    }
    const slot: u32 = @as(u32, index) * c.k_mbr_part_entry_stride;
    if (buf[c.k_mbr_off_part0_type + slot] == 0) {
        return c.k_ra8_err_not_found;
    }
    const base = std.mem.readInt(u32, buf[c.k_mbr_off_part0_lba + slot ..][0..4], .little);
    if (base == 0) {
        return c.k_ra8_err_validation_failed;
    }
    out_base.* = base;
    return c.k_ra8_ok;
}

/// Resolve an explicit partition index to its first LBA.
///
/// Dispatches on the sector-0 partition table held in the module scratch:
/// a GPT disk (protective MBR type 0xEE) selects GPT entry `index`, any
/// other 0xAA55 MBR selects primary entry `index`. A sector 0 with no boot
/// signature is not a partition table at all, so an explicit index has
/// nothing to select and the superfloppy request is refused. The signature
/// bytes are tested separately, mirroring `internal_mbr_part0_lba`.
export fn internal_locate_indexed(m: [*c]const c.ra8_fs_mount_t, index: u8, out_base: [*c]u64) callconv(.c) c.ra8_err_t {
    const st = @import("ra8_fs_fat_state_abi.zig");
    if (st.g_fs_scratch[c.k_bpb_off_signature_lo] != c.k_bpb_sig_lo) {
        return c.k_ra8_err_not_found;
    }
    if (st.g_fs_scratch[c.k_bpb_off_signature_hi] != c.k_bpb_sig_hi) {
        return c.k_ra8_err_not_found;
    }
    if (st.g_fs_scratch[c.k_mbr_off_part0_type] == c.k_gpt_part_type_protective) {
        return c.priv_gpt_locate_partition(m, index, out_base);
    }
    return internal_mbr_select_entry(st.g_fs_scratch[0..], index, out_base);
}

/// Read the volume's boot sector into the module scratch, resolving the
/// partition base first when `index` selects a partition explicitly.
///
/// The auto path (`k_ra8_fs_partition_auto`) tries a superfloppy parse at
/// LBA 0; when that fails it falls back to partition 0's start LBA from the
/// MBR, re-locating through the GPT when the disk is GPT-partitioned, and
/// re-reads the boot sector from the resolved base. The indexed path
/// resolves the requested partition up front. Both paths end in a BPB
/// parse of the sector now in scratch.
export fn internal_read_boot_sector(m: [*c]c.ra8_fs_mount_t, index: u8) callconv(.c) c.ra8_err_t {
    const st = @import("ra8_fs_fat_state_abi.zig");
    var err = c.priv_read_sector(m, 0, &st.g_fs_scratch);
    if (err != c.k_ra8_ok) {
        return err;
    }
    var base: u64 = 0;
    if (index != c.k_ra8_fs_partition_auto) {
        const lerr = internal_locate_indexed(m, index, &base);
        if (lerr != c.k_ra8_ok) {
            return lerr;
        }
    } else {
        err = c.priv_parse_volume(m);
        if (err == c.k_ra8_ok) {
            return c.k_ra8_ok;
        }
        base = internal_mbr_part0_lba(&st.g_fs_scratch);
        if (base == 0) {
            return err;
        }
        if (st.g_fs_scratch[c.k_mbr_off_part0_type] == c.k_gpt_part_type_protective) {
            const gpt_err = c.priv_gpt_locate_volume(m, &base);
            if (gpt_err != c.k_ra8_ok) {
                return gpt_err;
            }
        }
    }
    m.*.partition_base_lba = base;
    err = c.priv_read_sector(m, 0, &st.g_fs_scratch);
    if (err != c.k_ra8_ok) {
        return err;
    }
    return c.priv_parse_volume(m);
}

/// Probe the backend's block size into a claimed mount slot.
///
/// The sector size is the DEVICE's block size, probed before the
/// boot-sector parse so the parse can hold the volume to it. The capacity
/// callback must answer, and the block size it reports must pass
/// `internal_bps_valid`; anything else is refused before a buffer is
/// sized from it.
export fn internal_mount_probe_bps(backend: [*c]const c.ra8_fs_backend_t, m: [*c]c.ra8_fs_mount_t) callconv(.c) c.ra8_err_t {
    var dev_blocks: u64 = 0;
    var dev_bps: u32 = 0;
    const cerr = backend.*.get_capacity.?(backend.*.ctx, &dev_blocks, &dev_bps);
    if (cerr != c.k_ra8_ok) {
        return cerr;
    }
    if (internal_bps_valid(dev_bps) == 0) {
        return c.k_ra8_err_invalid_arg;
    }
    m.*.bytes_per_sector = dev_bps;
    return c.k_ra8_ok;
}

/// Parse a backend into temporary state and return only its format type.
///
/// Runs the full mount path against a throwaway mount: probe the device
/// block size, read and parse the boot sector (auto partition), and derive
/// the geometry for FAT volumes. No mount slot is claimed and the medium
/// is never written; the caller holds the library lock.
export fn internal_probe_locked(backend: [*c]const c.ra8_fs_backend_t, out_type: [*c]c.ra8_fs_type_t) callconv(.c) c.ra8_err_t {
    if (backend == null) {
        return c.k_ra8_err_null_ptr;
    }
    if (out_type == null) {
        return c.k_ra8_err_null_ptr;
    }
    if (backend.*.read_block == null) {
        return c.k_ra8_err_invalid_arg;
    }
    if (backend.*.get_capacity == null) {
        return c.k_ra8_err_invalid_arg;
    }
    var probe: c.ra8_fs_mount_t = std.mem.zeroes(c.ra8_fs_mount_t);
    probe.backend = backend.*;
    var err = internal_mount_probe_bps(backend, &probe);
    if (err != c.k_ra8_ok) {
        return err;
    }
    err = internal_read_boot_sector(&probe, c.k_ra8_fs_partition_auto);
    if (err != c.k_ra8_ok) {
        return err;
    }
    if (probe.type != c.k_ra8_fs_type_exfat) {
        err = internal_compute_geometry(&probe);
        if (err != c.k_ra8_ok) {
            return err;
        }
    }
    out_type.* = probe.type;
    return c.k_ra8_ok;
}

/// Parse the boot sector in the module scratch into a mount.
///
/// Runs the mount-side validation first: the 0x55/0xAA boot signature, then
/// the BPB-vs-device agreement -- `m.bytes_per_sector` was seeded from the
/// backend's reported block size before this parse ran, and a volume
/// formatted for a different sector size than the medium presents (a 512e
/// image on a 4Kn device, or vice versa) is unmountable, not
/// reinterpretable. Every supported size passes `internal_bps_valid` at
/// mount, so the comparison also enforces 512..4096. Zero sectors-per-
/// cluster or zero FAT count refuse the volume before any derived field is
/// written.
///
/// Field order follows the MS FAT spec BPB layout; FAT size and total
/// sectors pick the 16-bit field when non-zero, else the 32-bit one, chosen
/// in 32 bits first so the composite pick is never narrowed then widened.
export fn priv_parse_bpb_into_mount(m: [*c]c.ra8_fs_mount_t) callconv(.c) c.ra8_err_t {
    const st = @import("ra8_fs_fat_state_abi.zig");
    if (st.g_fs_scratch[c.k_bpb_off_signature_lo] != c.k_bpb_sig_lo or
        st.g_fs_scratch[c.k_bpb_off_signature_hi] != c.k_bpb_sig_hi)
    {
        return c.k_ra8_err_validation_failed;
    }
    const bpb_bps: u32 = c.priv_rd16(&st.g_fs_scratch[c.k_bpb_off_bytes_per_sec]);
    m.*.sectors_per_cluster = st.g_fs_scratch[c.k_bpb_off_sec_per_clus];
    m.*.reserved_sectors = c.priv_rd16(&st.g_fs_scratch[c.k_bpb_off_rsvd_sec_cnt]);
    m.*.num_fats = st.g_fs_scratch[c.k_bpb_off_num_fats];
    m.*.root_entries = c.priv_rd16(&st.g_fs_scratch[c.k_bpb_off_root_ent_cnt]);
    if (bpb_bps != m.*.bytes_per_sector or
        m.*.sectors_per_cluster == 0 or
        m.*.num_fats == 0)
    {
        return c.k_ra8_err_validation_failed;
    }
    const fat_sz_16: u32 = c.priv_rd16(&st.g_fs_scratch[c.k_bpb_off_fat_sz_16]);
    const fat_sz_32: u32 = c.priv_rd32(&st.g_fs_scratch[c.k_bpb_off_fat_sz_32]);
    const tot_sec_16: u32 = c.priv_rd16(&st.g_fs_scratch[c.k_bpb_off_tot_sec_16]);
    const tot_sec_32: u32 = c.priv_rd32(&st.g_fs_scratch[c.k_bpb_off_tot_sec_32]);
    m.*.fat_size_sectors = if (fat_sz_16 != 0) fat_sz_16 else fat_sz_32;
    const tot_sec: u32 = if (tot_sec_16 != 0) tot_sec_16 else tot_sec_32;
    m.*.total_sectors = tot_sec;
    m.*.root_cluster = c.priv_rd32(&st.g_fs_scratch[c.k_bpb_off_root_clus]);
    return c.k_ra8_ok;
}

/// Mount a volume: the guarded body of `ra8_fs_mount` / `ra8_fs_mount_partition`.
///
/// Allocates a mount slot and zeroes the WHOLE slot on claim: the mount
/// table is a fixed array, so the slot has very likely served another
/// volume already, and the FAT and exFAT parse paths each populate a
/// different subset of the struct. Clearing every field makes
/// "repopulate everything" a structural invariant instead of something
/// each future field author must remember. Same reasoning resets
/// `exfat_upcase_ok`: a fresh mount vouches for nothing until it has
/// looked. The allocator slot is claimed and seeded AFTER the volume
/// parses, so a failed parse leaves no allocator state behind. `in_use`
/// is set last, on success.
export fn internal_mount_locked(backend: [*c]const c.ra8_fs_backend_t, index: u8, out_handle: [*c][*c]c.ra8_fs_mount_t) callconv(.c) c.ra8_err_t {
    const st = @import("ra8_fs_fat_state_abi.zig");
    if (backend == null or out_handle == null) {
        return c.k_ra8_err_null_ptr;
    }
    if (backend.*.read_block == null or
        backend.*.write_block == null or
        backend.*.get_capacity == null)
    {
        return c.k_ra8_err_invalid_arg;
    }
    const m = st.internal_alloc_mount_slot();
    if (m == null) {
        return c.k_ra8_err_no_mem;
    }
    m.* = std.mem.zeroes(c.ra8_fs_mount_t);
    m.*.backend = backend.*;
    m.*.partition_base_lba = 0;
    const cerr = internal_mount_probe_bps(backend, m);
    if (cerr != c.k_ra8_ok) {
        return cerr;
    }
    m.*.exfat_upcase_ok = 0;
    var err = internal_read_boot_sector(m, index);
    if (err != c.k_ra8_ok) {
        return err;
    }
    // exFAT geometry is parsed directly in priv_exfat_parse; the FAT-BPB
    // geometry computation applies only to FAT12/16/32.
    if (m.*.type != c.k_ra8_fs_type_exfat) {
        err = internal_compute_geometry(m);
        if (err != c.k_ra8_ok) {
            return err;
        }
    }
    c.priv_alloc_state_bind(m);
    err = c.priv_fsinfo_seed(m);
    if (err != c.k_ra8_ok) {
        c.priv_alloc_state_release(m);
        return err;
    }
    m.*.in_use = 1;
    out_handle.* = m;
    return c.k_ra8_ok;
}

/// Release a mount slot: the guarded body of `ra8_fs_unmount`.
///
/// Last chance to make the on-disk free count true. The FSInfo flush is
/// reported, not swallowed: a card whose FSInfo could not be updated is a
/// card a desktop will report the wrong free space for, and the caller is
/// the only one who can decide whether that matters. The slot is released
/// either way, so a failed flush cannot strand the mount.
export fn internal_unmount_locked(handle: [*c]c.ra8_fs_mount_t) callconv(.c) c.ra8_err_t {
    if (handle == null) {
        return c.k_ra8_err_null_ptr;
    }
    if (handle.*.in_use == 0) {
        return c.k_ra8_err_invalid_state;
    }
    const ferr = c.priv_fsinfo_flush(handle);
    c.priv_alloc_state_release(handle);
    handle.*.in_use = 0;
    handle.*.type = c.k_ra8_fs_type_unknown;
    return ferr;
}

/// Lay down a fresh volume: the guarded body of `ra8_fs_format`.
///
/// Validates the request (type supported, sectors-per-cluster legal,
/// backend writable and readable for capacity), reads the device's block
/// count and size, and dispatches: exFAT goes to its own formatter, FAT
/// 12/16/32 go through the shared geometry builder -- root entries come
/// from the FAT12/16 constant (zero for FAT32, whose root lives in a
/// cluster chain), and the root-directory sector count is the entry table
/// rounded up to whole sectors. The final geometry choice and the volume
/// write stay with the fmt helpers.
export fn internal_format_locked(backend: [*c]const c.ra8_fs_backend_t, opts: [*c]const c.ra8_fs_format_opts_t) callconv(.c) c.ra8_err_t {
    if (backend == null or opts == null) {
        return c.k_ra8_err_null_ptr;
    }
    if (backend.*.write_block == null or
        backend.*.get_capacity == null)
    {
        return c.k_ra8_err_invalid_arg;
    }
    if (opts.*.type != c.k_ra8_fs_type_fat12 and
        opts.*.type != c.k_ra8_fs_type_fat16 and
        opts.*.type != c.k_ra8_fs_type_fat32 and
        opts.*.type != c.k_ra8_fs_type_exfat)
    {
        return c.k_ra8_err_not_supported;
    }
    if (!c.priv_fmt_spc_valid(opts.*.sectors_per_cluster)) {
        return c.k_ra8_err_invalid_arg;
    }
    var block_count: u64 = 0;
    var block_size: u32 = 0;
    var err = backend.*.get_capacity.?(backend.*.ctx, &block_count, &block_size);
    if (err != c.k_ra8_ok) {
        return err;
    }
    if (internal_bps_valid(block_size) == 0 or block_count == 0) {
        return c.k_ra8_err_invalid_arg;
    }
    if (opts.*.type == c.k_ra8_fs_type_exfat) {
        return c.priv_exfat_format(backend, block_count, block_size, opts.*.label);
    }
    var geom: c.ra8_fs_fmt_geom_t = std.mem.zeroes(c.ra8_fs_fmt_geom_t);
    geom.type = opts.*.type;
    geom.total_sectors = block_count;
    geom.bytes_per_sector = block_size;
    geom.reserved_sectors = c.priv_fmt_reserved_for(opts.*.type);
    geom.root_entries = if (opts.*.type == c.k_ra8_fs_type_fat32) 0 else @as(u32, c.k_fmt_root_ents_f16);
    geom.root_sectors =
        ((geom.root_entries * @as(u32, c.k_ra8_fs_dir_entry_bytes)) + (block_size - 1)) / block_size;
    err = c.priv_fmt_choose_geometry(&geom, opts.*.sectors_per_cluster);
    if (err != c.k_ra8_ok) {
        return err;
    }
    return c.priv_fmt_emit_volume(backend, &geom, opts.*.label);
}
