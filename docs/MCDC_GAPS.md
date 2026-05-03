# MC/DC Coverage Gap Audit

Live audit of compound boolean decisions reported by `llvm-cov show --show-mcdc` for first-party sources (`libs/`, `src/`, `port/`, excluding `libs/third_party/`). Regenerated from `build/mcdc-report/mcdc.txt` by `scripts/utils/regen_mcdc_gaps.py`; do not edit by hand.

## Methodology

- Source of truth: `build/mcdc-report/mcdc.txt` (output of `make mcdc`).
- A decision is one llvm-cov "MC/DC Decision Region". Condition count is taken from the `Number of Conditions:` field that llvm-cov emits for that region.
- Coverage status (`covered` column):
  - `yes` -- llvm-cov reports 100.00% MC/DC for the decision. Excluded from the CSV (CSV is gap-only).
  - `partial` -- 0 < MC/DC % < 100. The decision was exercised but at least one independence pair is missing.
  - `no` -- MC/DC % == 0. The decision was never evaluated under instrumentation.

## Top-line Numbers

- Source files with at least one decision: **93**
- Total compound decisions in scope: **539**
- Decisions at 100% MC/DC (`yes`): **411**
- Decisions partially covered (`partial`): **66**
- Decisions fully uncovered (`no`): **62**
- Coverage rate (yes / total): **76.25%**
- Deactivated gap conditions (DO-178C 6.4.4.3): **8**
- Reachable-condition denominator (total - deactivated): **531**
- **Reachable MC/DC rate**: **77.40%** -- this is the gate threshold (100% required).

See `docs/MCDC_DEACTIVATIONS.md` for the per-condition deactivation rationale catalog.

## Reachable gaps (require new MC/DC test vectors)

| File | Line | Conds | Function | Excerpt | Status |
|------|-----:|------:|----------|---------|--------|
| libs/ra_ble_host/src/ra_ble_att.c | 385 | 2 | internal_handle_find_info | `if ((start == 0U) \|\| (start > end)) {` | no |
| libs/ra_ble_host/src/ra_ble_att.c | 404 | 2 | internal_handle_find_info | `if ((a->handle < start) \|\| (a->handle > end)) {` | no |
| libs/ra_ble_host/src/ra_ble_att.c | 492 | 2 | internal_handle_read_by_type | `if ((a->handle < start) \|\| (a->handle > end)) {` | no |
| libs/ra_ble_host/src/ra_ble_att.c | 658 | 2 | internal_handle_read | `} else if ((a->kind == k_attr_kind_char_value) && (a->value != NULL)) {` | no |
| libs/ra_ble_host/src/ra_ble_gatt.c | 480 | 2 | ra_ble_host_gatt_set_value | `if ((len > 0U) && (a->value != NULL)) {` | partial |
| libs/ra_ble_host/src/ra_ble_gatt.c | 538 | 2 | ra_ble_host_gatt_notify | `if ((decl == NULL) \|\| ((decl->props & (uint8_t)k_ra_ble_host_char_prop_noti...` | partial |
| libs/ra_ble_host/src/ra_ble_gatt.c | 569 | 2 | ra_ble_host_gatt_notify | `if ((value_len > 0U) && (a->value != NULL)) {` | partial |
| libs/ra_ble_host/src/ra_ble_l2cap.c | 576 | 2 | ra_ble_host_acl_in | `if ((params == NULL) \|\| (s_state.initialized == 0U)) {` | no |
| libs/ra_ble_host/src/ra_ble_l2cap.c | 580 | 3 | ra_ble_host_acl_in | `if ((evt_code == k_evt_le_meta) && (params_len >= k_min_lemeta_param_bytes) &&` | no |
| libs/ra_ble_host/src/ra_ble_l2cap.c | 597 | 2 | ra_ble_host_acl_in | `} else if ((evt_code == k_evt_disconn_complete) && (params_len >= k_min_disco...` | no |
| libs/ra_board_ek_ra8d2/src/ra_board_ek_ra8d2.c | 975 | 2 | ra_board_audio_init | `if (channels != (uint8_t)k_ra_audio_channels_mono &&` | partial |
| libs/ra_core/src/ra_log.c | 260 | 2 | internal_itm_put_u32 | `while (value != 0U && i < k_ra_u32_max_digits) {` | partial |
| libs/ra_epub/src/ra_epub_chapter.c | 225 | 2 | priv_font_init | `if (w < 0 \|\| h < 0) {` | no |
| libs/ra_epub/src/ra_epub_chapter.c | 300 | 2 | ra_epub_get_chapter_count | `if (book->in_use == 0U \|\| book->zip_archive_active == 0U) {` | partial |
| libs/ra_epub/src/ra_epub_chapter.c | 369 | 2 | ra_epub_get_metadata | `if (book->in_use == 0U \|\| book->zip_archive_active == 0U) {` | partial |
| libs/ra_epub/src/ra_epub_open.c | 157 | 2 | priv_dirname | `if (dst == NULL \|\| cap == 0U) {` | no |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 101 | 2 | (file scope) | `if (dst == nullptr \|\| cap == 0U) {` | no |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 109 | 2 | (file scope) | `while (i + 1U < cap && src[i] != '\0') {` | partial |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 124 | 2 | (file scope) | `if (root == nullptr \|\| local_name == nullptr) {` | no |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 150 | 2 | (file scope) | `if (parent == nullptr \|\| local_name == nullptr) {` | no |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 173 | 2 | (file scope) | `if (manifest == nullptr \|\| id == nullptr) {` | no |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 179 | 2 | (file scope) | `if (item_id != nullptr && std::strcmp(item_id, id) == 0) {` | partial |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 197 | 2 | (file scope) | `if (props != nullptr && std::strstr(props, "cover-image") != nullptr) {` | partial |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 225 | 3 | (file scope) | `if (meta_name != nullptr && meta_content != nullptr && std::strcmp(meta_name,...` | no |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 241 | 2 | (file scope) | `if (xml_bytes == nullptr \|\| out == nullptr) {` | no |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 268 | 2 | (file scope) | `if (full_path == nullptr \|\| full_path[0] == '\0') {` | no |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 278 | 2 | (file scope) | `if (xml_bytes == nullptr \|\| book == nullptr) {` | no |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 314 | 2 | (file scope) | `if (manifest == nullptr \|\| spine == nullptr) {` | no |
| libs/ra_fs/src/ra_fs_fat.c | 939 | 2 | priv_path_to_83 | `if (path == NULL \|\| out11 == NULL) {` | no |
| libs/ra_fs/src/ra_fs_fat.c | 993 | 3 | priv_83_to_str | `if (j > 0 && (uint8_t)out12[0] == k_dir_marker_kanji_e5 && in11[0] == k_dir_m...` | no |
| libs/ra_fs/src/ra_fs_fat.c | 1281 | 2 | priv_free_chain | `while (cur >= k_cluster_first_data && (cur - k_cluster_first_data) < m->count...` | no |
| libs/ra_fs/src/ra_fs_fat.c | 2476 | 3 | ra_fs_listdir | `if (path[0] != '/' \|\| (path[0] == '/' && path[1] != '\0')) {` | partial |
| libs/ra_hal/src/ra_ble.c | 458 | 2 | internal_dispatch_event | `if ((internal_rx_byte(&code) == 0U) \|\| (internal_rx_byte(&plen) == 0U)) {` | no |
| libs/ra_hal/src/ra_ble.c | 492 | 4 | internal_dispatch_acl | `if ((internal_rx_byte(&hdl_lo) == 0U) \|\| (internal_rx_byte(&hdl_hi) == 0U) ...` | no |
| libs/ra_hal/src/ra_canfd.c | 313 | 2 | ra_canfd_deinit | `if ((bitrate_bps == 0U) \|\| (clock_hz == 0U)) {` | partial |
| libs/ra_hal/src/ra_canfd.c | 322 | 2 | ra_canfd_deinit | `if ((prescaler < k_ra_canfd_prescaler_min) \|\| (prescaler > prescaler_max)) {` | partial |
| libs/ra_hal/src/ra_ceu.c | 974 | 2 | internal_arm_capture | `if ((s_ceu_image_area != 0U) && (bufs->y_top != nullptr)) {` | partial |
| libs/ra_hal/src/ra_dmac.c | 151 | 2 | internal_dts_code | `if (mode == k_ra_dmac_mode_normal \|\| mode == k_ra_dmac_mode_repeat_block) {` | partial |
| libs/ra_hal/src/ra_dmac.c | 246 | 2 | internal_dmint_value | `if (cfg->irq_each && cfg->mode != k_ra_dmac_mode_repeat_block) {` | no |
| libs/ra_hal/src/ra_dotf.c | 361 | 2 | internal_check_overlap | `if ((region->start_addr <= live->end_addr) && (live->start_addr <= region->en...` | no |
| libs/ra_hal/src/ra_drw.c | 776 | 2 | internal_pack_texture_bits | `if ((uint16_t)rect->width_px < k_ra_drw_min_dim_px \|\|` | partial |
| libs/ra_hal/src/ra_drw.c | 780 | 2 | internal_pack_texture_bits | `if ((uint16_t)rect->width_px > k_ra_drw_max_width_px \|\|` | partial |
| libs/ra_hal/src/ra_eth.c | 329 | 2 | internal_init_rings | `if ((tx == 0U) \|\| (tx > k_ra_eth_num_tx_desc)) {` | no |
| libs/ra_hal/src/ra_eth.c | 332 | 2 | internal_init_rings | `if ((rx == 0U) \|\| (rx > k_ra_eth_num_rx_desc)) {` | no |
| libs/ra_hal/src/ra_flash.c | 149 | 2 | internal_wait_buffer_ready | `if (((s & k_ra_mrcps_mask_prgbsyc) == 0U) && ((s & k_ra_mrcps_mask_abuffull) ...` | no |
| libs/ra_hal/src/ra_flash.c | 180 | 2 | internal_wait_commit_done | `if (((s & k_ra_mrcps_mask_abufemp) != 0U) && ((s & k_ra_mrcps_mask_prgbsyc) =...` | no |
| libs/ra_hal/src/ra_flash.c | 722 | 2 | internal_window_allows | `if (s_rt.win_low == 0U && s_rt.win_high == 0U) {` | no |
| libs/ra_hal/src/ra_flash.c | 1389 | 2 | ra_flash_config_set_write | `(bool)((target_addr >= (uint32_t)k_ra_flash_extra_start) && (target_addr < ex...` | no |
| libs/ra_hal/src/ra_flash.c | 1390 | 2 | ra_flash_config_set_write | `if (!in_ofs && !in_extra) {` | partial |
| libs/ra_hal/src/ra_flash.c | 2698 | 2 | ra_flash_blank_check | `(address >= k_ra_flash_code_start) &&` | partial |
| libs/ra_hal/src/ra_flash.c | 2701 | 2 | ra_flash_blank_check | `(address >= k_ra_flash_extra_start) &&` | no |
| libs/ra_hal/src/ra_flash.c | 2707 | 3 | ra_flash_blank_check | `if (!in_code && !in_extra && !in_ofs) {` | partial |
| libs/ra_hal/src/ra_i3c.c | 688 | 2 | ra_i3c_send_ccc | `if ((target_addr > (uint8_t)k_ra_i3c_addr_mask) \|\| (max_len == 0U)) {` | no |
| libs/ra_hal/src/ra_i3c.c | 815 | 3 | ra_i3c_set_hdr_mode | `if ((mode != k_ra_i3c_hdr_mode_sdr) && (mode != k_ra_i3c_hdr_mode_ddr) &&` | partial |
| libs/ra_hal/src/ra_iic_b.c | 128 | 2 | internal_iic_b_half_period | `if ((bus_hz == 0U) \|\| (pclka_hz == 0U)) {` | no |
| libs/ra_hal/src/ra_iic_b.c | 975 | 2 | ra_iic_b_read | `if ((rx_len != 0U) && (rx == nullptr)) {` | partial |
| libs/ra_hal/src/ra_iic_b.c | 1234 | 2 | ra_iic_b_dispatch_eri | `if ((mask != 0U) && (cb != nullptr)) {` | partial |
| libs/ra_hal/src/ra_jpeg_sw.c | 987 | 2 | dec_parse_dqt | `if (len < 2U \|\| (uint32_t)len > d->src_len - d->cursor) {` | partial |
| libs/ra_hal/src/ra_jpeg_sw.c | 1031 | 2 | dec_parse_dht | `if (len < 2U \|\| (uint32_t)len > d->src_len - d->cursor) {` | partial |
| libs/ra_hal/src/ra_jpeg_sw.c | 1041 | 2 | dec_parse_dht | `if (tc >= (uint8_t)k_ra_jpeg_huff_classes \|\| th >= (uint8_t)k_ra_jpeg_huff_...` | no |
| ... | | | | *(60 more rows in CSV)* | |

## Deactivated gaps (DO-178C 6.4.4.3 exempted)

These conditions are unreachable on any public-API path and are therefore exempted from the MC/DC gate. Each row carries the rationale used by the auto-classifier; humans may extend the per-condition narrative in `docs/MCDC_DEACTIVATIONS.md`.

| File | Line | Conds | Function | Excerpt | Rationale |
|------|-----:|------:|----------|---------|-----------|
| libs/ra_hal/src/ra_rsip.c | 2958 | 2 | internal_hash_pull_digest | `if ((msg == nullptr) && (msg_len != 0U)) {` | Defensive null+len contract: (ptr == NULL) && (len != 0) ... |
| libs/ra_hal/src/ra_rsip.c | 3921 | 2 | internal_kw_pull_handle | `if ((label == nullptr) && (label_len != 0U)) {` | Defensive null+len contract: (ptr == NULL) && (len != 0) ... |
| libs/ra_hal/src/ra_rsip.c | 3924 | 2 | internal_kw_pull_handle | `if ((salt == nullptr) && (salt_len != 0U)) {` | Defensive null+len contract: (ptr == NULL) && (len != 0) ... |
| libs/ra_psa_crypto/src/ra_psa_crypto.c | 476 | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < key_len) && (off < sizeof(buf));...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra_psa_crypto/src/ra_psa_crypto.c | 479 | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < nonce_len) && (off < sizeof(buf)...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra_psa_crypto/src/ra_psa_crypto.c | 482 | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < aad_len) && (off < sizeof(buf));...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra_psa_crypto/src/ra_psa_crypto.c | 524 | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < key_len) && (off < sizeof(seed))...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra_psa_crypto/src/ra_psa_crypto.c | 527 | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < nonce_len) && (off < sizeof(seed...` | Defensive scratch-buffer bound: input length is capped by... |

## Per-module gap counts (full table)

Sorted by (uncovered + partial) descending, then total descending.

| Module | Total | Covered | Partial | Uncovered |
|--------|------:|--------:|--------:|----------:|
| ra_jpeg_sw | 24 | 9 | 9 | 6 |
| ra_epub_xml_shim | 12 | 0 | 3 | 9 |
| ra_flash | 20 | 12 | 3 | 5 |
| ra_reflow_layout | 14 | 7 | 4 | 3 |
| ra_modem_at | 12 | 6 | 5 | 1 |
| ra_mipi_dsi | 22 | 17 | 3 | 2 |
| ra_psa_crypto | 21 | 16 | 5 | 0 |
| ra_rsip | 16 | 11 | 2 | 3 |
| ra_fs_fat | 20 | 16 | 1 | 3 |
| ra_ble_att | 5 | 1 | 0 | 4 |
| ra_reflow_xml_shim | 4 | 0 | 2 | 2 |
| ra_epub_chapter | 13 | 10 | 2 | 1 |
| ra_touch_cal | 13 | 10 | 3 | 0 |
| ra_ble_l2cap | 12 | 9 | 0 | 3 |
| ra_ble_gatt | 11 | 8 | 3 | 0 |
| ra_net_udp | 9 | 6 | 2 | 1 |
| ra_iic_b | 7 | 4 | 2 | 1 |
| ra_ble | 8 | 6 | 0 | 2 |
| ra_ota | 8 | 6 | 1 | 1 |
| ra_usb_pal | 8 | 6 | 2 | 0 |
| ra_drw | 7 | 5 | 2 | 0 |
| ra_i3c | 7 | 5 | 1 | 1 |
| ra_lvd | 5 | 3 | 2 | 0 |
| ra_canfd | 4 | 2 | 2 | 0 |
| ra_eth | 4 | 2 | 0 | 2 |
| ra_dmac | 3 | 1 | 1 | 1 |
| ra_rmac_phy | 3 | 1 | 0 | 2 |
| ra_mipi_phy | 22 | 21 | 1 | 0 |
| ra_vin | 14 | 13 | 1 | 0 |
| ra_spi_b | 9 | 8 | 0 | 1 |
| ra_dotf | 6 | 5 | 0 | 1 |
| ra_usb_cdc | 4 | 3 | 0 | 1 |
| ra_usb_hmsc | 4 | 3 | 0 | 1 |
| ra_epub_open | 3 | 2 | 0 | 1 |
| ra_board_ek_ra8d2 | 2 | 1 | 1 | 0 |
| ra_ceu | 2 | 1 | 1 | 0 |
| ra_net_pal | 2 | 1 | 0 | 1 |
| ra_rmac | 2 | 1 | 0 | 1 |
| ra_wdt_supervisor | 2 | 1 | 1 | 0 |
| ra_log | 1 | 0 | 1 | 0 |
| ra_net_arp | 1 | 0 | 0 | 1 |
| ra_reflow_render | 1 | 0 | 0 | 1 |
| ra_etha | 14 | 14 | 0 | 0 |
| ra_gfx_text | 12 | 12 | 0 | 0 |
| ra_usb | 11 | 11 | 0 | 0 |
| ra_ssie | 10 | 10 | 0 | 0 |
| ra_pdg | 8 | 8 | 0 | 0 |
| ra_sci | 8 | 8 | 0 | 0 |
| ra_mpu | 7 | 7 | 0 | 0 |
| ra_usb_paud | 6 | 6 | 0 | 0 |
| ra_usb_phid | 6 | 6 | 0 | 0 |
| ra_net_ipv4 | 5 | 5 | 0 | 0 |
| ra_usb_haud | 5 | 5 | 0 | 0 |
| ra_usb_hcdc_ecm | 5 | 5 | 0 | 0 |
| ra_usb_pprn | 5 | 5 | 0 | 0 |
| ra_ble_gatt_client | 4 | 4 | 0 | 0 |
| ra_tls | 4 | 4 | 0 | 0 |
| ra_usb_hhid | 4 | 4 | 0 | 0 |
| ra_usb_pvnd | 4 | 4 | 0 | 0 |
| ra_xspi | 4 | 4 | 0 | 0 |
| ra_gpt | 3 | 3 | 0 | 0 |
| ra_net_tcp | 3 | 3 | 0 | 0 |
| adc | 2 | 2 | 0 | 0 |
| ra_bkup | 2 | 2 | 0 | 0 |
| ra_ble_mesh | 2 | 2 | 0 | 0 |
| ra_ble_patch | 2 | 2 | 0 | 0 |
| ra_iic_b_slave | 2 | 2 | 0 | 0 |
| ra_ipc | 2 | 2 | 0 | 0 |
| ra_touch | 2 | 2 | 0 | 0 |
| ra_tsn | 2 | 2 | 0 | 0 |
| ra_usb_composite | 2 | 2 | 0 | 0 |
| ra_usb_hcdc | 2 | 2 | 0 | 0 |
| ra_usb_hhub | 2 | 2 | 0 | 0 |
| ra_vreg | 2 | 2 | 0 | 0 |
| secure_trng | 2 | 2 | 0 | 0 |
| key_import | 1 | 1 | 0 | 0 |
| ota_commit | 1 | 1 | 0 | 0 |
| ra_ble_security | 1 | 1 | 0 | 0 |
| ra_crc | 1 | 1 | 0 | 0 |
| ra_epaper | 1 | 1 | 0 | 0 |
| ra_ether_phy | 1 | 1 | 0 | 0 |
| ra_glcdc | 1 | 1 | 0 | 0 |
| ra_isr | 1 | 1 | 0 | 0 |
| ra_mipi_csi | 1 | 1 | 0 | 0 |
| ra_nsc_eth | 1 | 1 | 0 | 0 |
| ra_nsc_ota | 1 | 1 | 0 | 0 |
| ra_nsc_xspi | 1 | 1 | 0 | 0 |
| ra_ptp | 1 | 1 | 0 | 0 |
| ra_pwr | 1 | 1 | 0 | 0 |
| ra_reflow_parse | 1 | 1 | 0 | 0 |
| ra_smbus | 1 | 1 | 0 | 0 |
| ra_sram | 1 | 1 | 0 | 0 |
| ra_usb_pmsc | 1 | 1 | 0 | 0 |

## Top 30 modules with at least one uncovered decision

| Module | Uncovered | Partial | Covered | Total |
|--------|----------:|--------:|--------:|------:|
| ra_epub_xml_shim | 9 | 3 | 0 | 12 |
| ra_jpeg_sw | 6 | 9 | 9 | 24 |
| ra_flash | 5 | 3 | 12 | 20 |
| ra_ble_att | 4 | 0 | 1 | 5 |
| ra_reflow_layout | 3 | 4 | 7 | 14 |
| ra_rsip | 3 | 2 | 11 | 16 |
| ra_fs_fat | 3 | 1 | 16 | 20 |
| ra_ble_l2cap | 3 | 0 | 9 | 12 |
| ra_mipi_dsi | 2 | 3 | 17 | 22 |
| ra_reflow_xml_shim | 2 | 2 | 0 | 4 |
| ra_ble | 2 | 0 | 6 | 8 |
| ra_eth | 2 | 0 | 2 | 4 |
| ra_rmac_phy | 2 | 0 | 1 | 3 |
| ra_modem_at | 1 | 5 | 6 | 12 |
| ra_epub_chapter | 1 | 2 | 10 | 13 |
| ra_iic_b | 1 | 2 | 4 | 7 |
| ra_net_udp | 1 | 2 | 6 | 9 |
| ra_dmac | 1 | 1 | 1 | 3 |
| ra_i3c | 1 | 1 | 5 | 7 |
| ra_ota | 1 | 1 | 6 | 8 |
| ra_dotf | 1 | 0 | 5 | 6 |
| ra_epub_open | 1 | 0 | 2 | 3 |
| ra_net_arp | 1 | 0 | 0 | 1 |
| ra_net_pal | 1 | 0 | 1 | 2 |
| ra_reflow_render | 1 | 0 | 0 | 1 |
| ra_rmac | 1 | 0 | 1 | 2 |
| ra_spi_b | 1 | 0 | 8 | 9 |
| ra_usb_cdc | 1 | 0 | 3 | 4 |
| ra_usb_hmsc | 1 | 0 | 3 | 4 |

---

*Regenerated from the live `make mcdc` report. See `docs/MCDC_GAPS.csv` for the full per-decision table including line numbers and decision excerpts.*
