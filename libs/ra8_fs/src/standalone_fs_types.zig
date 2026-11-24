//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Standalone `fs_h` constant and struct definitions for syntax checking.

pub const k_ra8_ok: u16 = 0;
pub const k_ra8_err_no_mem: u16 = 258;
pub const k_ra8_err_invalid_arg: u16 = 259;
pub const k_ra8_err_invalid_state: u16 = 260;
pub const k_ra8_err_invalid_size: u16 = 261;
pub const k_ra8_err_not_found: u16 = 262;
pub const k_ra8_err_not_supported: u16 = 263;
pub const k_ra8_err_busy: u16 = 265;
pub const k_ra8_err_exists: u16 = 268;
pub const k_ra8_err_not_empty: u16 = 273;
pub const k_ra8_err_access_denied: u16 = 274;
pub const k_ra8_err_out_of_range: u16 = 520;
pub const k_ra8_err_protocol_error: u16 = 1030;
pub const k_ra8_err_validation_failed: u16 = 1281;
pub const k_ra8_err_null_ptr: u16 = 1284;
pub const ra8_err_codes_t = u16;
pub const ra8_err_t = ra8_err_codes_t;
pub const k_ra8_fs_max_files: u8 = 4;
pub const k_ra8_fs_max_mounts: u8 = 2;
pub const k_ra8_fs_short_name_len: u8 = 12;
pub const k_ra8_fs_sector_min: u16 = 512;
pub const k_ra8_fs_sector_max: u16 = 4096;
pub const k_ra8_fs_dir_entry_bytes: u16 = 32;
pub const k_ra8_fs_dir_state_bytes: u16 = 640;
pub const k_ra8_fs_fat_max_file_bytes: c_uint = 4294967295;
pub const k_ra8_fs_mode_read: u8 = 0;
pub const k_ra8_fs_mode_write: u8 = 1;
pub const k_ra8_fs_mode_append: u8 = 2;
pub const ra8_fs_mode_t = u8;
pub const k_ra8_fs_attr_read_only: u8 = 1;
pub const k_ra8_fs_attr_volume_id: u8 = 8;
pub const k_ra8_fs_attr_directory: u8 = 16;
pub const k_ra8_fs_attr_archive: u8 = 32;
pub const k_ra8_fs_attr_lfn: u8 = 15;
pub const k_ra8_fs_type_unknown: u8 = 0;
pub const k_ra8_fs_type_fat12: u8 = 12;
pub const k_ra8_fs_type_fat16: u8 = 16;
pub const k_ra8_fs_type_fat32: u8 = 32;
pub const k_ra8_fs_type_exfat: u8 = 64;
pub const ra8_fs_type_t = u8;
pub const ra8_fs_format_opts_t = extern struct {
    type: ra8_fs_type_t,
    label: [*c]const u8,
    sectors_per_cluster: u8,
};
pub const ra8_fs_dirent_t = extern struct {
    name: [742]u8,
    size_bytes: u64,
    attr: u8,
};
pub const ra8_fs_datetime_t = extern struct {
    year: u16,
    utc_offset_min: i16,
    month: u8,
    day: u8,
    hour: u8,
    minute: u8,
    second: u8,
    centisecond: u8,
};
pub const ra8_fs_timestamp_t = extern struct {
    value: ra8_fs_datetime_t,
    valid: bool,
    utc_offset_valid: bool,
};
pub const ra8_fs_stat_t = extern struct {
    size_bytes: u64,
    first_cluster: u32,
    created: ra8_fs_timestamp_t,
    modified: ra8_fs_timestamp_t,
    accessed: ra8_fs_timestamp_t,
    attr: u8,
    is_directory: bool,
};
pub const ra8_fs_listdir_cb_t = ?*const fn (name: [*c]const u8, attr: u8, size: u64, ctx: ?*anyopaque) callconv(.c) void;
pub const k_ra8_fs_partition_auto: u8 = 255;
pub const k_fs_free_unknown: c_uint = 4294967295;
pub const k_fs_alloc_slots: c_uint = 2;
pub const k_fs_fsinfo_absent: c_uint = 0;
pub const k_fs_cache_empty: c_ulong = 18446744073709551615;
pub const k_fs_bitmap_unknown: c_ulong = 18446744073709551615;
pub const k_bpb_off_bytes_per_sec: u16 = 11;
pub const k_bpb_off_sec_per_clus: u16 = 13;
pub const k_bpb_off_rsvd_sec_cnt: u16 = 14;
pub const k_bpb_off_num_fats: u16 = 16;
pub const k_bpb_off_root_ent_cnt: u16 = 17;
pub const k_bpb_off_tot_sec_16: u16 = 19;
pub const k_bpb_off_fat_sz_16: u16 = 22;
pub const k_bpb_off_tot_sec_32: u16 = 32;
pub const k_bpb_off_fat_sz_32: u16 = 36;
pub const k_bpb_off_root_clus: u16 = 44;
pub const k_bpb_off_signature_lo: u16 = 510;
pub const k_bpb_off_signature_hi: u16 = 511;
pub const k_dir_off_name: u8 = 0;
pub const k_dir_off_attr: u8 = 11;
pub const k_dir_off_ntres: u8 = 12;
pub const k_dir_off_fst_clus_hi: u8 = 20;
pub const k_dir_off_fst_clus_lo: u8 = 26;
pub const k_dir_off_file_size: u8 = 28;
pub const k_dir_name_field_len: u8 = 11;
pub const k_ntres_base_lower: u8 = 8;
pub const k_ntres_ext_lower: u8 = 16;
pub const k_dir_marker_free_perm: u8 = 0;
pub const k_dir_marker_free_used: u8 = 229;
pub const k_dir_marker_kanji_e5: u8 = 5;
pub const k_dir_marker_dot: u8 = 46;
pub const k_bpb_sig_lo: u8 = 85;
pub const k_bpb_sig_hi: u8 = 170;
pub const k_mbr_off_part0_type: u16 = 450;
pub const k_mbr_off_part0_lba: u16 = 454;
pub const k_mbr_part_entry_stride: u16 = 16;
pub const k_mbr_part_entry_count: u16 = 4;
pub const k_gpt_part_type_protective: u16 = 238;
pub const k_gpt_header_lba: u16 = 1;
pub const k_gpt_off_entry_lba: u16 = 72;
pub const k_gpt_off_entry_count: u16 = 80;
pub const k_gpt_off_entry_size: u16 = 84;
pub const k_gpt_entry_bytes: u16 = 128;
pub const k_gpt_entry_scan_max: u16 = 128;
pub const k_gpt_entry_off_first_lba: u16 = 32;
pub const k_gpt_guid_len: u16 = 16;
pub const k_exfat_off_fsname: u16 = 3;
pub const k_exfat_off_fat_lba: u16 = 80;
pub const k_exfat_off_fat_len: u16 = 84;
pub const k_exfat_off_heap_lba: u16 = 88;
pub const k_exfat_off_clus_count: u16 = 92;
pub const k_exfat_off_root_clus: u16 = 96;
pub const k_exfat_off_bps_shift: u16 = 108;
pub const k_exfat_off_spc_shift: u16 = 109;
pub const k_exfat_off_num_fats: u16 = 110;
pub const k_exfat_strm_off_flags: u16 = 1;
pub const k_exfat_strm_off_nlen: u16 = 3;
pub const k_exfat_strm_off_clus: u16 = 20;
pub const k_exfat_strm_off_dlen: u16 = 24;
pub const k_exfat_name_off: u16 = 2;
pub const k_exfat_upc_off_csum: u16 = 4;
pub const k_exfat_entry_eod: c_uint = 0;
pub const k_exfat_entry_bitmap: c_uint = 129;
pub const k_exfat_entry_file: c_uint = 133;
pub const k_exfat_entry_stream: c_uint = 192;
pub const k_exfat_entry_name: c_uint = 193;
pub const k_exfat_secflag_no_fat: c_uint = 2;
pub const k_exfat_secflag_poss: c_uint = 1;
pub const k_exfat_secflag_alloc: c_uint = 3;
pub const k_exfat_attr_read_only: c_uint = 1;
pub const k_exfat_attr_directory: c_uint = 16;
pub const k_exfat_attr_archive: c_uint = 32;
pub const k_exfat_fsname_len: c_uint = 8;
pub const k_exfat_name_per_entry: c_uint = 15;
pub const k_exfat_entry_bytes: c_uint = 32;
pub const k_exfat_bps_shift_min: c_uint = 9;
pub const k_exfat_bps_shift_max: c_uint = 12;
pub const k_exfat_scan_limit: c_uint = 65536;
pub const k_exfat_name_cap: c_uint = 64;
pub const k_exfat_name_u8_cap: c_uint = 193;
pub const k_exfat_upc_words: c_uint = 2918;
pub const k_exfat_upc_run_tag: c_uint = 65535;
pub const k_exfat_csum_hi_bit: c_uint = 32768;
pub const k_exfat_off_file_secnt: c_uint = 1;
pub const k_exfat_off_file_csum: c_uint = 2;
pub const k_exfat_off_file_attr: c_uint = 4;
pub const k_exfat_off_strm_hash: c_uint = 4;
pub const k_exfat_off_strm_valid: c_uint = 8;
pub const k_exfat_bit_mask: c_uint = 7;
pub const k_exfat_bit_shift: c_uint = 3;
pub const k_exfat_inuse_bit: c_uint = 128;
pub const k_exfat_max_set_bytes: c_uint = 224;
pub const k_exfat_path_depth: c_uint = 32;
pub const k_exfat_dir_grow_max: c_uint = 2;
pub const k_exfat_foff_jump: u16 = 0;
pub const k_exfat_foff_part_off: u16 = 64;
pub const k_exfat_foff_vol_len: u16 = 72;
pub const k_exfat_foff_serial: u16 = 100;
pub const k_exfat_foff_fs_rev: u16 = 104;
pub const k_exfat_foff_drive: u16 = 111;
pub const k_exfat_foff_percent: u16 = 112;
pub const k_exfat_foff_ext_sig: u16 = 508;
pub const k_exfat_foff_boot_sig: u16 = 510;
pub const k_exfat_de_second: u16 = 32;
pub const k_exfat_de_third: u16 = 64;
pub const k_exfat_de_first_clus: u16 = 20;
pub const k_exfat_de_data_len: u16 = 24;
pub const k_exfat_de_upc_csum: u16 = 4;
pub const k_exfat_de_lbl_cnt: u16 = 1;
pub const k_exfat_de_lbl_name: u16 = 2;
pub const k_exfat_entry_upcase: c_uint = 130;
pub const k_exfat_entry_label: c_uint = 131;
pub const k_exfat_fmt_jump0: c_uint = 235;
pub const k_exfat_fmt_jump1: c_uint = 118;
pub const k_exfat_fmt_jump2: c_uint = 144;
pub const k_exfat_fmt_fs_rev: c_uint = 256;
pub const k_exfat_fmt_drive: c_uint = 128;
pub const k_exfat_fmt_percent: c_uint = 0;
pub const k_exfat_fmt_boot_sig: c_uint = 43605;
pub const k_exfat_fmt_ext_sig: c_uint = 2857697280;
pub const k_exfat_fmt_fat_media: c_uint = 4294967288;
pub const k_exfat_fmt_fat_eoc: c_uint = 4294967295;
pub const k_exfat_fmt_first_clus: c_uint = 2;
pub const k_exfat_fmt_num_fats: c_uint = 1;
pub const k_exfat_fmt_boot_secs: c_uint = 24;
pub const k_exfat_fmt_backup_lba: c_uint = 12;
pub const k_exfat_fmt_ext_first: c_uint = 1;
pub const k_exfat_fmt_ext_count: c_uint = 8;
pub const k_exfat_fmt_oem_lba: c_uint = 9;
pub const k_exfat_fmt_resv_lba: c_uint = 10;
pub const k_exfat_fmt_csum_lba: c_uint = 11;
pub const k_exfat_fmt_csum_skip0: c_uint = 106;
pub const k_exfat_fmt_csum_skip2: c_uint = 112;
pub const k_exfat_fmt_csum_hibit: c_uint = 2147483648;
pub const k_exfat_fmt_min_sectors: c_uint = 65536;
pub const k_exfat_fmt_thr_256m: c_uint = 524288;
pub const k_exfat_fmt_thr_32g: c_uint = 67108864;
pub const k_exfat_fmt_thr_256g: c_uint = 536870912;
pub const k_exfat_fmt_clus_4k: c_uint = 12;
pub const k_exfat_fmt_clus_32k: c_uint = 15;
pub const k_exfat_fmt_clus_128k: c_uint = 17;
pub const k_exfat_fmt_clus_256k: c_uint = 18;
pub const k_exfat_fmt_geom_iters: c_uint = 4;
pub const k_exfat_fmt_upc_std_bytes: c_uint = 5836;
pub const k_exfat_fmt_part_align: c_uint = 1048576;
pub const k_exfat_fmt_serial: c_uint = 1386800250;
pub const k_exfat_fmt_label_max: c_uint = 11;
pub const k_exfat_fmt_byte_bits: c_uint = 8;
pub const k_exfat_fmt_byte_full: c_uint = 255;
pub const k_mbr_fmt_disk_sig_off: c_uint = 440;
pub const k_mbr_fmt_part0_off: c_uint = 446;
pub const k_mbr_fmt_pe_boot: c_uint = 0;
pub const k_mbr_fmt_pe_chs_start: c_uint = 1;
pub const k_mbr_fmt_pe_type: c_uint = 4;
pub const k_mbr_fmt_pe_chs_end: c_uint = 5;
pub const k_mbr_fmt_pe_lba: c_uint = 8;
pub const k_mbr_fmt_pe_nsect: c_uint = 12;
pub const k_mbr_fmt_boot_none: c_uint = 0;
pub const k_mbr_fmt_type_exfat: c_uint = 7;
pub const k_mbr_fmt_chs_heads: c_uint = 255;
pub const k_mbr_fmt_chs_spt: c_uint = 63;
pub const k_mbr_fmt_chs_max: c_uint = 16450560;
pub const k_mbr_fmt_chs_ovf_h: c_uint = 254;
pub const k_mbr_fmt_chs_ovf_m: c_uint = 255;
pub const k_mbr_fmt_chs_ovf_l: c_uint = 255;
pub const k_mbr_fmt_chs_sec_mask: c_uint = 63;
pub const k_mbr_fmt_chs_cyl_hi_mask: c_uint = 768;
pub const k_mbr_fmt_chs_cyl_hi_shift: c_uint = 2;
pub const k_mbr_fmt_disk_sig_base: c_uint = 439041101;
pub const k_cluster_first_data: u32 = 2;
pub const k_cluster_eoc_min_fat12: u32 = 4088;
pub const k_cluster_eoc_min_fat16: u32 = 65528;
pub const k_cluster_eoc_min_fat32: u32 = 268435448;
pub const k_cluster_mask_fat32: c_uint = 268435455;
pub const k_cluster_eoc_min_exfat: c_uint = 4294967288;
pub const k_cluster_eoc_write_fat12: c_uint = 4095;
pub const k_cluster_eoc_write_fat16: c_uint = 65535;
pub const k_cluster_eoc_write_fat32: c_uint = 268435455;
pub const k_cluster_eoc_write_exfat: c_uint = 4294967295;
pub const k_cluster_free: c_uint = 0;
pub const k_cluster_count_fat12_max: c_uint = 4085;
pub const k_cluster_count_fat16_max: c_uint = 65525;
pub const k_fat12_value_mask: c_uint = 4095;
pub const k_fat12_low_nibble_mask: c_uint = 15;
pub const k_fat12_high_nibble_mask: c_uint = 61440;
pub const k_fmt_off_jmp0: u16 = 0;
pub const k_fmt_off_jmp1: u16 = 1;
pub const k_fmt_off_jmp2: u16 = 2;
pub const k_fmt_off_oem: u16 = 3;
pub const k_fmt_off_media: u16 = 21;
pub const k_fmt_off_sec_per_trk: u16 = 24;
pub const k_fmt_off_num_heads: u16 = 26;
pub const k_fmt_off_f16_drvnum: u16 = 36;
pub const k_fmt_off_f16_bootsig: u16 = 38;
pub const k_fmt_off_f16_volid: u16 = 39;
pub const k_fmt_off_f16_label: u16 = 43;
pub const k_fmt_off_f16_fstype: u16 = 54;
pub const k_fmt_off_f32_fsinfo: u16 = 48;
pub const k_fmt_off_f32_bkboot: u16 = 50;
pub const k_fmt_off_f32_drvnum: u16 = 64;
pub const k_fmt_off_f32_bootsig: u16 = 66;
pub const k_fmt_off_f32_volid: u16 = 67;
pub const k_fmt_off_f32_label: u16 = 71;
pub const k_fmt_off_f32_fstype: u16 = 82;
pub const k_fmt_fsi_off_lead: u16 = 0;
pub const k_fmt_fsi_off_struct: u16 = 484;
pub const k_fmt_fsi_off_free: u16 = 488;
pub const k_fmt_fsi_off_nxtfree: u16 = 492;
pub const k_fmt_fsi_off_trail: u16 = 508;
pub const k_fmt_jmp_byte0: c_uint = 235;
pub const k_fmt_jmp_byte1: c_uint = 88;
pub const k_fmt_jmp_byte2: c_uint = 144;
pub const k_fmt_media_fixed: c_uint = 248;
pub const k_fmt_drvnum_hd: c_uint = 128;
pub const k_fmt_ext_bootsig: c_uint = 41;
pub const k_fmt_sec_per_trk: c_uint = 63;
pub const k_fmt_num_heads: c_uint = 255;
pub const k_fmt_num_fats: c_uint = 2;
pub const k_fmt_root_ents_f16: c_uint = 512;
pub const k_fmt_resv_f16: c_uint = 1;
pub const k_fmt_resv_f32: c_uint = 32;
pub const k_fmt_root_clus_f32: c_uint = 2;
pub const k_fmt_fsinfo_sector: c_uint = 1;
pub const k_fmt_bkboot_sector: c_uint = 6;
pub const k_fmt_volid_base: c_uint = 1386795520;
pub const k_fmt_fsi_lead_sig: c_uint = 1096897106;
pub const k_fmt_fsi_struct_sig: c_uint = 1631679090;
pub const k_fmt_fsi_trail_sig: c_uint = 2857697280;
pub const k_fmt_label_len: c_uint = 11;
pub const k_fmt_spc_max: c_uint = 64;
pub const k_fmt_fat32_entry_bytes: c_uint = 4;
pub const k_fmt_fat16_entry_bytes: c_uint = 2;
pub const k_fmt_fat32_clus_cap: c_uint = 268435440;
pub const k_fmt_fsi_unknown: c_uint = 4294967295;
pub const k_fmt_fat32_nxt_free: c_uint = 3;
pub const k_fmt_f32_thr_260m: c_uint = 532480;
pub const k_fmt_f32_thr_8g: c_uint = 16777216;
pub const k_fmt_f32_thr_16g: c_uint = 33554432;
pub const k_fmt_f32_thr_32g: c_uint = 67108864;
pub const k_fmt_f32_clus_512b: c_uint = 9;
pub const k_fmt_f32_clus_4k: c_uint = 12;
pub const k_fmt_f32_clus_8k: c_uint = 13;
pub const k_fmt_f32_clus_16k: c_uint = 14;
pub const k_fmt_f32_clus_32k: c_uint = 15;
pub const k_byte_mask: u16 = 255;
pub const k_word_mask: u16 = 65535;
pub const k_max_8_3_name: u16 = 11;
pub const k_filename_base_len: u16 = 8;
pub const k_filename_ext_len: u16 = 3;
pub const dir_walk_t = extern struct {
    is_root_fixed: u8,
    fixed_remaining: u32,
    cluster: u32,
    sector_in_cluster: u32,
    cur_lba: u64,
    entry_idx: u32,
    cluster_hops: u32,
};
pub const dir_loc_t = extern struct {
    is_root: u8,
    cluster: u32,
};
pub const k_lfn_off_seq: c_uint = 0;
pub const k_lfn_off_checksum: c_uint = 13;
pub const k_lfn_seq_order_mask: c_uint = 31;
pub const k_lfn_seq_last: c_uint = 64;
pub const k_lfn_chars_per_ent: c_uint = 13;
pub const k_lfn_max_entries: c_uint = 19;
pub const k_lfn_utf8_cap: c_uint = 742;
pub const k_lfn_unicode_pad: c_uint = 65535;
pub const k_lfn_write_max: c_uint = 247;
pub const k_lfn_erase_max: c_uint = 20;
pub const k_lfn_alias_tail_max: c_uint = 999999;
pub const k_name_kind_invalid: u8 = 0;
pub const k_name_kind_short: u8 = 1;
pub const k_name_kind_long: u8 = 2;
pub const ra8_fs_name_kind_t = u8;
pub const dir_pos_t = extern struct {
    lba: u64,
    off: u32,
};
pub const dir_slot_t = extern struct {
    w: dir_walk_t,
    ent: u32,
};
pub const dir_target_t = extern struct {
    parent: dir_loc_t,
    lba: u64,
    off: u32,
    entry: [32]u8,
};
pub const dir_insert_t = extern struct {
    start: dir_slot_t,
    units: [247]u16,
    nunits: u32,
    name83: [11]u8,
    ntres: u8,
    lfn_entries: u8,
};
pub const exfat_cursor_t = extern struct {
    cluster: u32,
    entry_in_cluster: u32,
    scanned: u32,
    contig_end: u32,
};
pub const k_exfat_set_max_entries: c_uint = 19;
pub const k_exfat_set_writable: c_uint = 7;
pub const k_exfat_set_min_entries: c_uint = 3;
pub const exfat_setpos_t = extern struct {
    cluster: u32,
    index: u32,
};
pub const k_dir_off_crt_time_tenth: u8 = 13;
pub const k_dir_off_crt_time: u8 = 14;
pub const k_dir_off_crt_date: u8 = 16;
pub const k_dir_off_lst_acc_date: u8 = 18;
pub const k_dir_off_wrt_time: u8 = 22;
pub const k_dir_off_wrt_date: u8 = 24;
pub const k_exfat_off_file_ctime: u8 = 8;
pub const k_exfat_off_file_mtime: u8 = 12;
pub const k_exfat_off_file_atime: u8 = 16;
pub const k_exfat_off_file_c10ms: u8 = 20;
pub const k_exfat_off_file_m10ms: u8 = 21;
pub const k_exfat_off_file_cutc: u8 = 22;
pub const k_exfat_off_file_mutc: u8 = 23;
pub const k_exfat_off_file_autc: u8 = 24;
pub const k_fs_time_epoch_year: c_uint = 1980;
pub const k_fs_time_year_max: c_uint = 2107;
pub const k_fs_time_month_min: c_uint = 1;
pub const k_fs_time_month_max: c_uint = 12;
pub const k_fs_time_day_min: c_uint = 1;
pub const k_fs_time_day_max: c_uint = 31;
pub const k_fs_time_hour_max: c_uint = 23;
pub const k_fs_time_minute_max: c_uint = 59;
pub const k_fs_time_second_max: c_uint = 59;
pub const k_fs_time_centi_max: c_uint = 99;
pub const k_fs_date_shift_year: c_uint = 9;
pub const k_fs_date_shift_month: c_uint = 5;
pub const k_fs_time_shift_hour: c_uint = 11;
pub const k_fs_time_shift_minute: c_uint = 5;
pub const k_fs_time_second_div: c_uint = 2;
pub const k_fs_xf_shift_date: c_uint = 16;
pub const k_fs_xf_shift_month: c_uint = 5;
pub const k_fs_xf_shift_year: c_uint = 9;
pub const k_fs_centi_per_second: c_uint = 100;
pub const k_fs_utc_step_min: c_uint = 15;
pub const k_fs_utc_valid_bit: c_uint = 128;
pub const k_fs_utc_field_mask: c_uint = 127;
pub const k_fs_utc_unknown: c_uint = 0;
pub const k_fs_utc_span_min: i16 = -720;
pub const k_fs_utc_span_max: i16 = 840;
pub const k_utf_byte_mask: c_uint = 255;
pub const k_utf_byte_shift: c_uint = 8;
pub const ra8_fs_space_t = extern struct {
    total_bytes: u64,
    free_bytes: u64,
    used_bytes: u64,
    bytes_per_cluster: u32,
    total_clusters: u32,
    free_clusters: u32,
    used_clusters: u32,
};
pub const k_ra8_fs_attr_settable: u8 = 39;
pub const k_ra8_fs_check_unknown: c_uint = 4294967295;
pub const k_ra8_fs_check_fault_bad_fat_value: u8 = 1;
pub const k_ra8_fs_check_fault_crosslink: u8 = 2;
pub const k_ra8_fs_check_fault_lost_cluster: u8 = 3;
pub const k_ra8_fs_check_fault_bad_chain: u8 = 4;
pub const k_ra8_fs_check_fault_bad_dir_entry: u8 = 5;
pub const k_ra8_fs_check_fault_bad_set_checksum: u8 = 6;
pub const k_ra8_fs_check_fault_bad_name_hash: u8 = 7;
pub const k_ra8_fs_check_fault_bitmap_ref_unset: u8 = 8;
pub const k_ra8_fs_check_fault_free_count_bad: u8 = 9;
pub const k_ra8_fs_check_fault_scan_truncated: u8 = 10;
pub const ra8_fs_check_fault_kind_t = u8;
pub const ra8_fs_check_fault_t = extern struct {
    kind: ra8_fs_check_fault_kind_t,
    cluster: u32,
    lba: u64,
    entry_off: u32,
};
pub const ra8_fs_check_report_t = extern struct {
    type: ra8_fs_type_t,
    referenced_scan: bool,
    clusters_total: u32,
    clusters_free: u32,
    clusters_used: u32,
    clusters_bad: u32,
    clusters_lost: u32,
    chains_crosslinked: u32,
    bitmap_mismatches: u32,
    entries_bad: u32,
    dirs_visited: u32,
    files_visited: u32,
    faults_total: u32,
    first_fault: ra8_fs_check_fault_t,
};
pub const k_ra8_fs_check_max_dirs: u16 = 128;
pub const fat_dir_stack_t = extern struct {
    items: [128]dir_loc_t,
    top: u32,
    truncated: u8,
};
