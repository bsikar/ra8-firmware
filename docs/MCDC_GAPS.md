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
- Total compound decisions in scope: **538**
- Decisions at 100% MC/DC (`yes`): **434**
- Decisions partially covered (`partial`): **59**
- Decisions fully uncovered (`no`): **45**
- Coverage rate (yes / total): **80.67%**
- Deactivated gap conditions (DO-178C 6.4.4.3): **27**
- Reachable-condition denominator (total - deactivated): **511**
- **Reachable MC/DC rate**: **84.93%** -- this is the gate threshold (100% required).

See `docs/MCDC_DEACTIVATIONS.md` for the per-condition deactivation rationale catalog.

## Reachable gaps (require new MC/DC test vectors)

| File | Line | Conds | Function | Excerpt | Status |
|------|-----:|------:|----------|---------|--------|
| libs/ra_ble_host/src/ra_ble_att.c | 385 | 2 | internal_handle_find_info | `if ((start == 0U) \|\| (start > end)) {` | no |
| libs/ra_ble_host/src/ra_ble_att.c | 404 | 2 | internal_handle_find_info | `if ((a->handle < start) \|\| (a->handle > end)) {` | partial |
| libs/ra_ble_host/src/ra_ble_att.c | 492 | 2 | internal_handle_read_by_type | `if ((a->handle < start) \|\| (a->handle > end)) {` | partial |
| libs/ra_ble_host/src/ra_ble_att.c | 658 | 2 | internal_handle_read | `} else if ((a->kind == k_attr_kind_char_value) && (a->value != NULL)) {` | no |
| libs/ra_ble_host/src/ra_ble_gatt.c | 480 | 2 | ra_ble_host_gatt_set_value | `if ((len > 0U) && (a->value != NULL)) {` | partial |
| libs/ra_ble_host/src/ra_ble_gatt.c | 538 | 2 | ra_ble_host_gatt_notify | `if ((decl == NULL) \|\| ((decl->props & (uint8_t)k_ra_ble_host_char_prop_noti...` | partial |
| libs/ra_ble_host/src/ra_ble_gatt.c | 569 | 2 | ra_ble_host_gatt_notify | `if ((value_len > 0U) && (a->value != NULL)) {` | partial |
| libs/ra_ble_host/src/ra_ble_l2cap.c | 576 | 2 | ra_ble_host_acl_in | `if ((params == NULL) \|\| (s_state.initialized == 0U)) {` | no |
| libs/ra_ble_host/src/ra_ble_l2cap.c | 580 | 3 | ra_ble_host_acl_in | `if ((evt_code == k_evt_le_meta) && (params_len >= k_min_lemeta_param_bytes) &&` | no |
| libs/ra_ble_host/src/ra_ble_l2cap.c | 597 | 2 | ra_ble_host_acl_in | `} else if ((evt_code == k_evt_disconn_complete) && (params_len >= k_min_disco...` | no |
| libs/ra_board_ek_ra8d2/src/ra_board_ek_ra8d2.c | 975 | 2 | ra_board_audio_init | `if (channels != (uint8_t)k_ra_audio_channels_mono &&` | partial |
| libs/ra_epub/src/ra_epub_chapter.c | 225 | 2 | priv_font_init | `if (w < 0 \|\| h < 0) {` | no |
| libs/ra_epub/src/ra_epub_chapter.c | 300 | 2 | ra_epub_get_chapter_count | `if (book->in_use == 0U \|\| book->zip_archive_active == 0U) {` | partial |
| libs/ra_epub/src/ra_epub_chapter.c | 369 | 2 | ra_epub_get_metadata | `if (book->in_use == 0U \|\| book->zip_archive_active == 0U) {` | partial |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 109 | 2 | (file scope) | `while (i + 1U < cap && src[i] != '\0') {` | partial |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 179 | 2 | (file scope) | `if (item_id != nullptr && std::strcmp(item_id, id) == 0) {` | partial |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 197 | 2 | (file scope) | `if (props != nullptr && std::strstr(props, "cover-image") != nullptr) {` | partial |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 225 | 3 | (file scope) | `if (meta_name != nullptr && meta_content != nullptr && std::strcmp(meta_name,...` | no |
| libs/ra_hal/src/ra_ble.c | 458 | 2 | internal_dispatch_event | `if ((internal_rx_byte(&code) == 0U) \|\| (internal_rx_byte(&plen) == 0U)) {` | no |
| libs/ra_hal/src/ra_ble.c | 492 | 4 | internal_dispatch_acl | `if ((internal_rx_byte(&hdl_lo) == 0U) \|\| (internal_rx_byte(&hdl_hi) == 0U) ...` | no |
| libs/ra_hal/src/ra_canfd.c | 323 | 2 | ra_canfd_deinit | `if ((prescaler < k_ra_canfd_prescaler_min) \|\| (prescaler > prescaler_max)) {` | partial |
| libs/ra_hal/src/ra_ceu.c | 974 | 2 | internal_arm_capture | `if ((s_ceu_image_area != 0U) && (bufs->y_top != nullptr)) {` | partial |
| libs/ra_hal/src/ra_dmac.c | 151 | 2 | internal_dts_code | `if (mode == k_ra_dmac_mode_normal \|\| mode == k_ra_dmac_mode_repeat_block) {` | partial |
| libs/ra_hal/src/ra_dmac.c | 246 | 2 | internal_dmint_value | `if (cfg->irq_each && cfg->mode != k_ra_dmac_mode_repeat_block) {` | no |
| libs/ra_hal/src/ra_dotf.c | 361 | 2 | internal_check_overlap | `if ((region->start_addr <= live->end_addr) && (live->start_addr <= region->en...` | no |
| libs/ra_hal/src/ra_drw.c | 776 | 2 | internal_pack_texture_bits | `if ((uint16_t)rect->width_px < k_ra_drw_min_dim_px \|\|` | partial |
| libs/ra_hal/src/ra_drw.c | 780 | 2 | internal_pack_texture_bits | `if ((uint16_t)rect->width_px > k_ra_drw_max_width_px \|\|` | partial |
| libs/ra_hal/src/ra_flash.c | 150 | 2 | internal_wait_buffer_ready | `if (((s & k_ra_mrcps_mask_prgbsyc) == 0U) && ((s & k_ra_mrcps_mask_abuffull) ...` | no |
| libs/ra_hal/src/ra_flash.c | 181 | 2 | internal_wait_commit_done | `if (((s & k_ra_mrcps_mask_abufemp) != 0U) && ((s & k_ra_mrcps_mask_prgbsyc) =...` | no |
| libs/ra_hal/src/ra_flash.c | 2737 | 2 | ra_flash_blank_check | `(address >= k_ra_flash_code_start) &&` | partial |
| libs/ra_hal/src/ra_flash.c | 2740 | 2 | ra_flash_blank_check | `(address >= k_ra_flash_extra_start) &&` | no |
| libs/ra_hal/src/ra_flash.c | 2746 | 3 | ra_flash_blank_check | `if (!in_code && !in_extra && !in_ofs) {` | partial |
| libs/ra_hal/src/ra_i3c.c | 688 | 2 | ra_i3c_send_ccc | `if ((target_addr > (uint8_t)k_ra_i3c_addr_mask) \|\| (max_len == 0U)) {` | no |
| libs/ra_hal/src/ra_i3c.c | 815 | 3 | ra_i3c_set_hdr_mode | `if ((mode != k_ra_i3c_hdr_mode_sdr) && (mode != k_ra_i3c_hdr_mode_ddr) &&` | partial |
| libs/ra_hal/src/ra_iic_b.c | 976 | 2 | ra_iic_b_read | `if ((rx_len != 0U) && (rx == nullptr)) {` | partial |
| libs/ra_hal/src/ra_iic_b.c | 1235 | 2 | ra_iic_b_dispatch_eri | `if ((mask != 0U) && (cb != nullptr)) {` | partial |
| libs/ra_hal/src/ra_jpeg_sw.c | 1223 | 2 | dec_parse_sos | `if (r < 0 && t != 0) {` | no |
| libs/ra_hal/src/ra_jpeg_sw.c | 1400 | 2 | dec_parse_sos | `while (i < jpeg_len && jpeg_buf[i] == (uint8_t)k_ra_jpeg_marker_byte) {` | partial |
| libs/ra_hal/src/ra_jpeg_sw.c | 1409 | 2 | dec_parse_sos | `if (mk == (uint16_t)k_ra_jpeg_marker_soi \|\| mk == (uint16_t)k_ra_jpeg_marke...` | partial |
| libs/ra_hal/src/ra_jpeg_sw.c | 1434 | 4 | dec_parse_sos | `if (mk >= 0xFFC0U && mk <= 0xFFCFU && mk != (uint16_t)k_ra_jpeg_marker_dht &&...` | partial |
| libs/ra_hal/src/ra_jpeg_sw.c | 1655 | 4 | dec_decode_scan | `} else if (mk >= 0xFFC1U && mk <= 0xFFCFU && mk != (uint16_t)k_ra_jpeg_marker...` | partial |
| libs/ra_hal/src/ra_lvd.c | 494 | 2 | internal_validate_cfg | `if ((cfg->hysteresis == k_ra_lvd_hysteresis_hvd) &&` | partial |
| libs/ra_hal/src/ra_lvd.c | 533 | 2 | internal_compose_cr0 | `if ((cfg->response == k_ra_lvd_response_reset) \|\|` | partial |
| libs/ra_hal/src/ra_mipi_dsi.c | 1453 | 2 | ra_mipi_dsi_dispatch_receive | `if ((s_pending_rx_buffer != nullptr) && (s_pending_rx_len > 0U)) {` | partial |
| libs/ra_hal/src/ra_mipi_phy.c | 1139 | 3 | internal_mipi_phy_write_timing | `if ((tbl[i].mode == mode_flag) && (tbl[i].pclka == pclka) && (tbl[i].rate_max...` | partial |
| libs/ra_hal/src/ra_rmac.c | 1565 | 2 | ra_rmac_phy_auto_neg_start | `if (out_link->up && ((bmsr & (uint16_t)k_ra_rmac_phy_bmsr_an_done) != 0U)) {` | no |
| libs/ra_hal/src/ra_rmac_phy.c | 352 | 2 | ra_rmac_phy_link_status_get | `if ((err == k_ra_ok) && ((msr & k_ra_rmac_phy_msr_1000full) != 0U)) {` | no |
| libs/ra_hal/src/ra_rmac_phy.c | 356 | 2 | ra_rmac_phy_link_status_get | `if ((err == k_ra_ok) && ((msr & k_ra_rmac_phy_msr_1000half) != 0U)) {` | no |
| libs/ra_hal/src/ra_spi_b.c | 714 | 2 | internal_apply_bit_width | `if ((tx == nullptr) && (rx == nullptr)) {` | no |
| libs/ra_hal/src/ra_usb_cdc.c | 185 | 2 | internal_apply_line_coding | `if ((data == nullptr) \|\| (len < k_ra_cdc_line_coding_len)) {` | no |
| libs/ra_hal/src/ra_usb_hmsc.c | 1274 | 3 | internal_normalise_xfer_err | `if ((err == k_ra_ok) \|\| (err == k_ra_err_no_data) \|\| (err == k_ra_err_hw_...` | no |
| libs/ra_hal/src/ra_vin.c | 348 | 2 | internal_mc_rmw | `if (((mc_now & k_ra_vin_mc_me) != 0UL) \|\| ((fc_now & k_ra_vin_fc_cc) != 0UL...` | partial |
| libs/ra_modem_at/src/ra_modem_at.c | 227 | 2 | internal_reset_line | `if ((s_mod.cfg.line_buf != nullptr) && (s_mod.cfg.line_buf_len > 0U)) {` | no |
| libs/ra_modem_at/src/ra_modem_at.c | 345 | 3 | internal_dispatch_urc | `if ((expected_response == nullptr) \|\| (expected_response[0] == '\0') \|\|` | partial |
| libs/ra_modem_at/src/ra_modem_at.c | 534 | 3 | internal_effective_timeout | `if ((expected_response != nullptr) && (expected_response[0] != '\0') &&` | partial |
| libs/ra_modem_at/src/ra_modem_at.c | 625 | 2 | internal_pump_one | `if ((capture != nullptr) && (capture_len > 0U)) {` | partial |
| libs/ra_net/src/ra_net_arp.c | 154 | 2 | arp_insert | `if (s->arp[i].timestamp_ms != 0U && memcmp(s->arp[i].ip.bytes, ip->bytes, 4U)...` | no |
| libs/ra_net/src/ra_net_udp.c | 539 | 2 | ra_net_udp_internal_dns_consume_response | `if (pos < dlen && dns[pos] == 0U) {` | partial |
| libs/ra_net/src/ra_net_udp.c | 554 | 2 | ra_net_udp_internal_dns_consume_response | `while (pos < dlen && dns[pos] != 0U) {` | no |
| libs/ra_net/src/ra_net_udp.c | 657 | 3 | ra_net_dns_query | `while ((s->dns_pending != 0U) && (s->dns_rcode == 0U) && (poll_budget != 0U)) {` | partial |
| ... | | | | *(17 more rows in CSV)* | |

## Deactivated gaps (DO-178C 6.4.4.3 exempted)

These conditions are unreachable on any public-API path and are therefore exempted from the MC/DC gate. Each row carries the rationale used by the auto-classifier; humans may extend the per-condition narrative in `docs/MCDC_DEACTIVATIONS.md`.

| File | Line | Conds | Function | Excerpt | Rationale |
|------|-----:|------:|----------|---------|-----------|
| libs/ra_core/src/ra_log.c | 261 | 2 | internal_itm_put_u32 | `while (value != 0U && i < k_ra_u32_max_digits) {` | Annotated deactivation: digit-buffer bound; uint32_t max ... |
| libs/ra_epub/src/ra_epub_open.c | 157 | 2 | priv_dirname | `if (dst == NULL \|\| cap == 0U) {` | TU-local static helper `priv_dirname` -- defensive NULL g... |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 101 | 2 | (file scope) | `if (dst == nullptr \|\| cap == 0U) {` | TU-local static helper `copy_bounded` -- defensive NULL g... |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 124 | 2 | (file scope) | `if (root == nullptr \|\| local_name == nullptr) {` | TU-local static helper `find_descendant` -- defensive NUL... |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 150 | 2 | (file scope) | `if (parent == nullptr \|\| local_name == nullptr) {` | TU-local static helper `find_child` -- defensive NULL gua... |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 173 | 2 | (file scope) | `if (manifest == nullptr \|\| id == nullptr) {` | TU-local static helper `manifest_href_by_id` -- defensive... |
| libs/ra_fs/src/ra_fs_fat.c | 939 | 2 | priv_path_to_83 | `if (path == NULL \|\| out11 == NULL) {` | TU-local static helper `priv_path_to_83` -- defensive NUL... |
| libs/ra_fs/src/ra_fs_fat.c | 994 | 3 | priv_83_to_str | `if (j > 0 && (uint8_t)out12[0] == k_dir_marker_kanji_e5 &...` | Annotated deactivation: 3-condition AND on Shift-JIS kanj... |
| libs/ra_fs/src/ra_fs_fat.c | 1283 | 2 | priv_free_chain | `while (cur >= k_cluster_first_data && (cur - k_cluster_fi...` | Annotated deactivation: loop bound; `cur < k_cluster_firs... |
| libs/ra_fs/src/ra_fs_fat.c | 2478 | 3 | ra_fs_listdir | `if (path[0] != '/' \|\| (path[0] == '/' && path[1] != '\0...` | Structurally-redundant condition: `x == V` inside the sec... |
| libs/ra_hal/src/ra_canfd.c | 314 | 2 | ra_canfd_deinit | `if ((bitrate_bps == 0U) \|\| (clock_hz == 0U)) {` | Annotated deactivation: both args are validated by ra_can... |
| libs/ra_hal/src/ra_eth.c | 330 | 2 | internal_init_rings | `if ((tx == 0U) \|\| (tx > k_ra_eth_num_tx_desc)) {` | Annotated deactivation: tx normalized to nonzero above; f... |
| libs/ra_hal/src/ra_eth.c | 334 | 2 | internal_init_rings | `if ((rx == 0U) \|\| (rx > k_ra_eth_num_rx_desc)) {` | Annotated deactivation: rx normalized to nonzero above; f... |
| libs/ra_hal/src/ra_iic_b.c | 129 | 2 | internal_iic_b_half_period | `if ((bus_hz == 0U) \|\| (pclka_hz == 0U)) {` | Annotated deactivation: both args validated by ra_iic_b_i... |
| libs/ra_hal/src/ra_jpeg_sw.c | 987 | 2 | dec_parse_dqt | `if (len < 2U \|\| (uint32_t)len > d->src_len - d->cursor) {` | Defensive segment-length bound in a bounded parser: buffe... |
| libs/ra_hal/src/ra_jpeg_sw.c | 1031 | 2 | dec_parse_dht | `if (len < 2U \|\| (uint32_t)len > d->src_len - d->cursor) {` | Defensive segment-length bound in a bounded parser: buffe... |
| libs/ra_hal/src/ra_jpeg_sw.c | 1089 | 2 | dec_parse_sof0 | `if (len < 8U \|\| (uint32_t)len > d->src_len - d->cursor) {` | Defensive segment-length bound in a bounded parser: buffe... |
| libs/ra_hal/src/ra_jpeg_sw.c | 1161 | 2 | dec_parse_sos | `if (len < 6U \|\| (uint32_t)len > d->src_len - d->cursor) {` | Defensive segment-length bound in a bounded parser: buffe... |
| libs/ra_hal/src/ra_rsip.c | 2958 | 2 | internal_hash_pull_digest | `if ((msg == nullptr) && (msg_len != 0U)) {` | Defensive null+len contract: (ptr == NULL) && (len != 0) ... |
| libs/ra_hal/src/ra_rsip.c | 3921 | 2 | internal_kw_pull_handle | `if ((label == nullptr) && (label_len != 0U)) {` | Defensive null+len contract: (ptr == NULL) && (len != 0) ... |
| libs/ra_hal/src/ra_rsip.c | 3924 | 2 | internal_kw_pull_handle | `if ((salt == nullptr) && (salt_len != 0U)) {` | Defensive null+len contract: (ptr == NULL) && (len != 0) ... |
| libs/ra_modem_at/src/ra_modem_at.c | 126 | 2 | ra_modem_at_internal_str_len | `while ((i < UINT16_MAX) && (s[i] != '\0')) {` | Annotated deactivation: 64KB-bound is a defensive watchdo... |
| libs/ra_psa_crypto/src/ra_psa_crypto.c | 476 | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < key_len) && (off < sizeof(buf));...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra_psa_crypto/src/ra_psa_crypto.c | 479 | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < nonce_len) && (off < sizeof(buf)...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra_psa_crypto/src/ra_psa_crypto.c | 482 | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < aad_len) && (off < sizeof(buf));...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra_psa_crypto/src/ra_psa_crypto.c | 524 | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < key_len) && (off < sizeof(seed))...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra_psa_crypto/src/ra_psa_crypto.c | 527 | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < nonce_len) && (off < sizeof(seed...` | Defensive scratch-buffer bound: input length is capped by... |

## Per-module gap counts (full table)

Sorted by (uncovered + partial) descending, then total descending.

| Module | Total | Covered | Partial | Uncovered |
|--------|------:|--------:|--------:|----------:|
| ra_jpeg_sw | 24 | 15 | 6 | 3 |
| ra_epub_xml_shim | 12 | 4 | 3 | 5 |
| ra_psa_crypto | 21 | 16 | 5 | 0 |
| ra_flash | 20 | 15 | 2 | 3 |
| ra_reflow_layout | 13 | 8 | 4 | 1 |
| ra_modem_at | 12 | 7 | 4 | 1 |
| ra_fs_fat | 20 | 16 | 1 | 3 |
| ra_ble_att | 5 | 1 | 2 | 2 |
| ra_reflow_xml_shim | 4 | 0 | 2 | 2 |
| ra_rsip | 16 | 13 | 0 | 3 |
| ra_epub_chapter | 13 | 10 | 2 | 1 |
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
| ra_eth | 4 | 2 | 2 | 0 |
| ra_dmac | 3 | 1 | 1 | 1 |
| ra_rmac_phy | 3 | 1 | 0 | 2 |
| ra_mipi_dsi | 22 | 21 | 1 | 0 |
| ra_mipi_phy | 22 | 21 | 1 | 0 |
| ra_vin | 14 | 13 | 1 | 0 |
| ra_touch_cal | 13 | 12 | 1 | 0 |
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
| ra_epub_xml_shim | 5 | 3 | 4 | 12 |
| ra_jpeg_sw | 3 | 6 | 15 | 24 |
| ra_flash | 3 | 2 | 15 | 20 |
| ra_fs_fat | 3 | 1 | 16 | 20 |
| ra_ble_l2cap | 3 | 0 | 9 | 12 |
| ra_rsip | 3 | 0 | 13 | 16 |
| ra_ble_att | 2 | 2 | 1 | 5 |
| ra_reflow_xml_shim | 2 | 2 | 0 | 4 |
| ra_ble | 2 | 0 | 6 | 8 |
| ra_rmac_phy | 2 | 0 | 1 | 3 |
| ra_modem_at | 1 | 4 | 7 | 12 |
| ra_reflow_layout | 1 | 4 | 8 | 13 |
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
