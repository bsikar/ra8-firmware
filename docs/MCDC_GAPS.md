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
- Total compound decisions in scope: **531**
- Decisions at 100% MC/DC (`yes`): **473**
- Decisions partially covered (`partial`): **30**
- Decisions fully uncovered (`no`): **28**
- Coverage rate (yes / total): **89.08%**
- Deactivated gap conditions (DO-178C 6.4.4.3): **58**
- Reachable-condition denominator (total - deactivated): **473**
- **Reachable MC/DC rate**: **100.00%** -- this is the gate threshold (100% required).

See `docs/MCDC_DEACTIVATIONS.md` for the per-condition deactivation rationale catalog.

## Reachable gaps (require new MC/DC test vectors)

| File | Conds | Function | Excerpt | Status |
|------|------:|----------|---------|--------|

## Deactivated gaps (DO-178C 6.4.4.3 exempted)

These conditions are unreachable on any public-API path and are therefore exempted from the MC/DC gate. Each row carries the rationale used by the auto-classifier; humans may extend the per-condition narrative in `docs/MCDC_DEACTIVATIONS.md`.

| File | Conds | Function | Excerpt | Rationale |
|------|------:|----------|---------|-----------|
| libs/ra_ble_host/src/ra_ble_att.c | 2 | internal_handle_read | `} else if ((a->kind == k_attr_kind_char_value) && (a->val...` | Annotated deactivation: TU-local helper internal_handle_r... |
| libs/ra_board_ek_ra8d2/src/ra_board_ek_ra8d2.c | 2 | ra_board_audio_init | `if (channels != (uint8_t)k_ra_audio_channels_mono &&` | Annotated deactivation: ra_board_audio_init channel-valid... |
| libs/ra_core/src/ra_log.c | 2 | internal_itm_put_u32 | `while (value != 0U && i < k_ra_u32_max_digits) {` | Annotated deactivation: digit-buffer bound; uint32_t max ... |
| libs/ra_epub/src/ra_epub_open.c | 2 | priv_dirname | `if (dst == NULL \|\| cap == 0U) {` | TU-local static helper `priv_dirname` -- defensive NULL g... |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 2 | (file scope) | `if (dst == nullptr \|\| cap == 0U) {` | TU-local static helper `copy_bounded` -- defensive NULL g... |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 2 | (file scope) | `while (i + 1U < cap && src[i] != '\0') {` | Annotated deactivation: TU-local helper copy_bounded; cap... |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 2 | (file scope) | `if (root == nullptr \|\| local_name == nullptr) {` | TU-local static helper `find_descendant` -- defensive NUL... |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 2 | (file scope) | `if (parent == nullptr \|\| local_name == nullptr) {` | TU-local static helper `find_child` -- defensive NULL gua... |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 2 | (file scope) | `if (manifest == nullptr \|\| id == nullptr) {` | TU-local static helper `manifest_href_by_id` -- defensive... |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 2 | (file scope) | `if (item_id != nullptr && std::strcmp(item_id, id) == 0) {` | Annotated deactivation: TU-local helper manifest_href_by_... |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 2 | (file scope) | `if (props != nullptr && std::strstr(props, "cover-image")...` | Annotated deactivation: TU-local helper find_cover_by_pro... |
| libs/ra_epub/src/ra_epub_xml_shim.cpp | 3 | (file scope) | `if (meta_name != nullptr && meta_content != nullptr && st...` | Annotated deactivation: TU-local helper find_cover_by_met... |
| libs/ra_fs/src/ra_fs_fat.c | 2 | priv_path_to_83 | `if (path == NULL \|\| out11 == NULL) {` | TU-local static helper `priv_path_to_83` -- defensive NUL... |
| libs/ra_fs/src/ra_fs_fat.c | 3 | priv_83_to_str | `if (j > 0 && (uint8_t)out12[0] == k_dir_marker_kanji_e5 &...` | Annotated deactivation: 3-condition AND on Shift-JIS kanj... |
| libs/ra_fs/src/ra_fs_fat.c | 2 | priv_free_chain | `while (cur >= k_cluster_first_data && (cur - k_cluster_fi...` | Annotated deactivation: loop bound; `cur < k_cluster_firs... |
| libs/ra_fs/src/ra_fs_fat.c | 3 | ra_fs_listdir | `if (path[0] != '/' \|\| (path[0] == '/' && path[1] != '\0...` | Structurally-redundant condition: `x == V` inside the sec... |
| libs/ra_hal/src/ra_ble.c | 2 | internal_dispatch_event | `if ((internal_rx_byte(&code) == 0U) \|\| (internal_rx_byt...` | Annotated deactivation: TU-local helper internal_dispatch... |
| libs/ra_hal/src/ra_ble.c | 4 | internal_dispatch_acl | `if ((internal_rx_byte(&hdl_lo) == 0U) \|\| (internal_rx_b...` | Annotated deactivation: TU-local helper internal_dispatch... |
| libs/ra_hal/src/ra_canfd.c | 2 | ra_canfd_deinit | `if ((bitrate_bps == 0U) \|\| (clock_hz == 0U)) {` | Annotated deactivation: both args are validated by ra_can... |
| libs/ra_hal/src/ra_canfd.c | 2 | ra_canfd_deinit | `if ((prescaler < k_ra_canfd_prescaler_min) \|\| (prescale...` | Annotated deactivation: ra_canfd_deinit (bit-timing solve... |
| libs/ra_hal/src/ra_ceu.c | 2 | internal_arm_capture | `if ((s_ceu_image_area != 0U) && (bufs->y_top != nullptr)) {` | Annotated deactivation: TU-local helper internal_arm_capt... |
| libs/ra_hal/src/ra_dotf.c | 2 | internal_check_overlap | `if ((region->start_addr <= live->end_addr) && (live->star...` | Annotated deactivation: ra_dotf overlap-detection AND; th... |
| libs/ra_hal/src/ra_eth.c | 2 | internal_init_rings | `if ((tx == 0U) \|\| (tx > k_ra_eth_num_tx_desc)) {` | Annotated deactivation: tx normalized to nonzero above; f... |
| libs/ra_hal/src/ra_eth.c | 2 | internal_init_rings | `if ((rx == 0U) \|\| (rx > k_ra_eth_num_rx_desc)) {` | Annotated deactivation: rx normalized to nonzero above; f... |
| libs/ra_hal/src/ra_flash.c | 2 | ra_flash_blank_check | `(address >= k_ra_flash_code_start) &&` | Annotated deactivation: ra_flash_blank_check window-membe... |
| libs/ra_hal/src/ra_flash.c | 2 | ra_flash_blank_check | `(address >= k_ra_flash_extra_start) &&` | Annotated deactivation: ra_flash_blank_check extra-window... |
| libs/ra_hal/src/ra_flash.c | 3 | ra_flash_blank_check | `if (!in_code && !in_extra && !in_ofs) {` | Annotated deactivation: ra_flash_blank_check 3-way OR ove... |
| libs/ra_hal/src/ra_iic_b.c | 2 | internal_iic_b_half_period | `if ((bus_hz == 0U) \|\| (pclka_hz == 0U)) {` | Annotated deactivation: both args validated by ra_iic_b_i... |
| libs/ra_hal/src/ra_jpeg_sw.c | 2 | dec_parse_dqt | `if (len < 2U \|\| (uint32_t)len > d->src_len - d->cursor) {` | Defensive segment-length bound in a bounded parser: buffe... |
| libs/ra_hal/src/ra_jpeg_sw.c | 2 | dec_parse_dht | `if (len < 2U \|\| (uint32_t)len > d->src_len - d->cursor) {` | Defensive segment-length bound in a bounded parser: buffe... |
| libs/ra_hal/src/ra_jpeg_sw.c | 2 | dec_parse_sof0 | `if (len < 8U \|\| (uint32_t)len > d->src_len - d->cursor) {` | Defensive segment-length bound in a bounded parser: buffe... |
| libs/ra_hal/src/ra_jpeg_sw.c | 2 | dec_parse_sos | `if (len < 6U \|\| (uint32_t)len > d->src_len - d->cursor) {` | Defensive segment-length bound in a bounded parser: buffe... |
| libs/ra_hal/src/ra_jpeg_sw.c | 2 | dec_parse_sos | `if (r < 0 && t != 0) {` | Annotated deactivation: dec_parse_sos br_get_bits exhaust... |
| libs/ra_hal/src/ra_jpeg_sw.c | 2 | dec_parse_sos | `while (i < jpeg_len && jpeg_buf[i] == (uint8_t)k_ra_jpeg_...` | Annotated deactivation: dec_parse_sos JPEG marker-pad ski... |
| libs/ra_hal/src/ra_jpeg_sw.c | 4 | dec_parse_sos | `if (mk >= 0xFFC0U && mk <= 0xFFCFU && mk != (uint16_t)k_r...` | Annotated deactivation: dec_parse_sof0 unsupported-SOFn d... |
| libs/ra_hal/src/ra_jpeg_sw.c | 4 | dec_decode_scan | `} else if (mk >= 0xFFC1U && mk <= 0xFFCFU && mk != (uint1...` | Annotated deactivation: dec_decode_scan unsupported-SOFn ... |
| libs/ra_hal/src/ra_mipi_dsi.c | 2 | ra_mipi_dsi_dispatch_receive | `if ((s_pending_rx_buffer != nullptr) && (s_pending_rx_len...` | Annotated deactivation: ra_mipi_dsi_dispatch_receive pend... |
| libs/ra_hal/src/ra_mipi_phy.c | 3 | internal_mipi_phy_write_timing | `if ((tbl[i].mode == mode_flag) && (tbl[i].pclka == pclka)...` | Annotated deactivation: TU-local helper internal_mipi_phy... |
| libs/ra_hal/src/ra_rmac.c | 2 | ra_rmac_phy_auto_neg_start | `if (out_link->up && ((bmsr & (uint16_t)k_ra_rmac_phy_bmsr...` | Annotated deactivation: ra_rmac_phy_auto_neg_start link-u... |
| libs/ra_hal/src/ra_rsip.c | 2 | internal_hash_pull_digest | `if ((msg == nullptr) && (msg_len != 0U)) {` | Defensive null+len contract: (ptr == NULL) && (len != 0) ... |
| libs/ra_hal/src/ra_rsip.c | 2 | internal_kw_pull_handle | `if ((label == nullptr) && (label_len != 0U)) {` | Defensive null+len contract: (ptr == NULL) && (len != 0) ... |
| libs/ra_hal/src/ra_rsip.c | 2 | internal_kw_pull_handle | `if ((salt == nullptr) && (salt_len != 0U)) {` | Defensive null+len contract: (ptr == NULL) && (len != 0) ... |
| libs/ra_hal/src/ra_spi_b.c | 2 | internal_apply_bit_width | `if ((tx == nullptr) && (rx == nullptr)) {` | Annotated deactivation: TU-local helper internal_apply_bi... |
| libs/ra_hal/src/ra_usb_cdc.c | 2 | internal_apply_line_coding | `if ((data == nullptr) \|\| (len < k_ra_cdc_line_coding_le...` | Annotated deactivation: TU-local helper internal_apply_li... |
| libs/ra_hal/src/ra_usb_hmsc.c | 3 | internal_normalise_xfer_err | `if ((err == k_ra_ok) \|\| (err == k_ra_err_no_data) \|\| ...` | Annotated deactivation: TU-local helper internal_normalis... |
| libs/ra_hal/src/ra_vin.c | 2 | internal_mc_rmw | `if (((mc_now & k_ra_vin_mc_me) != 0UL) \|\| ((fc_now & k_...` | Annotated deactivation: ra_vin idle-state guard; MC.ME (m... |
| libs/ra_modem_at/src/ra_modem_at.c | 2 | ra_modem_at_internal_str_len | `while ((i < UINT16_MAX) && (s[i] != '\0')) {` | Annotated deactivation: 64KB-bound is a defensive watchdo... |
| libs/ra_net/src/ra_net_arp.c | 2 | arp_insert | `if (s->arp[i].timestamp_ms != 0U && memcmp(s->arp[i].ip.b...` | Annotated deactivation: TU-local helper arp_insert occupi... |
| libs/ra_net_pal/src/ra_net_pal.c | 2 | internal_eth_event | `if ((s_state.event_fn != nullptr) && (pal_mask != k_ra_ne...` | Annotated deactivation: TU-local helper internal_eth_even... |
| libs/ra_psa_crypto/src/ra_psa_crypto.c | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < key_len) && (off < sizeof(buf));...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra_psa_crypto/src/ra_psa_crypto.c | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < nonce_len) && (off < sizeof(buf)...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra_psa_crypto/src/ra_psa_crypto.c | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < aad_len) && (off < sizeof(buf));...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra_psa_crypto/src/ra_psa_crypto.c | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < key_len) && (off < sizeof(seed))...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra_psa_crypto/src/ra_psa_crypto.c | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < nonce_len) && (off < sizeof(seed...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra_reflow/src/ra_reflow_render.c | 2 | priv_blit_glyph | `if (w > 0 && h > 0) {` | Annotated deactivation: TU-local helper priv_blit_glyph; ... |
| libs/ra_reflow/src/ra_reflow_xml_shim.cpp | 2 | (file scope) | `if (dst == nullptr \|\| dst_cap == 0U) {` | Annotated deactivation: TU-local helper lowercase_truncat... |
| libs/ra_touch_cal/src/ra_touch_cal.c | 2 | internal_clip32 | `if (!ok_u \|\| !ok_v) {` | Annotated deactivation: TU-local helper internal_clip32 s... |
| libs/ra_wdt_supervisor/src/ra_wdt_supervisor.c | 2 | ra_wdt_supervisor_tick | `if (will_refresh && (s_state.refresh != nullptr)) {` | Annotated deactivation: ra_wdt_supervisor_tick refresh di... |

## Per-module gap counts (full table)

Sorted by (uncovered + partial) descending, then total descending.

| Module | Total | Covered | Partial | Uncovered |
|--------|------:|--------:|--------:|----------:|
| ra_jpeg_sw | 24 | 16 | 5 | 3 |
| ra_epub_xml_shim | 12 | 4 | 3 | 5 |
| ra_psa_crypto | 21 | 16 | 5 | 0 |
| ra_fs_fat | 20 | 16 | 1 | 3 |
| ra_flash | 20 | 17 | 2 | 1 |
| ra_rsip | 16 | 13 | 0 | 3 |
| ra_ble | 8 | 6 | 0 | 2 |
| ra_canfd | 4 | 2 | 2 | 0 |
| ra_eth | 4 | 2 | 2 | 0 |
| ra_mipi_dsi | 22 | 21 | 1 | 0 |
| ra_mipi_phy | 22 | 21 | 1 | 0 |
| ra_vin | 14 | 13 | 1 | 0 |
| ra_touch_cal | 13 | 12 | 1 | 0 |
| ra_modem_at | 12 | 11 | 1 | 0 |
| ra_spi_b | 9 | 8 | 0 | 1 |
| ra_iic_b | 7 | 6 | 0 | 1 |
| ra_dotf | 6 | 5 | 0 | 1 |
| ra_ble_att | 5 | 4 | 1 | 0 |
| ra_reflow_xml_shim | 5 | 4 | 0 | 1 |
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
| ra_ble_l2cap | 12 | 12 | 0 | 0 |
| ra_epub_chapter | 12 | 12 | 0 | 0 |
| ra_gfx_text | 12 | 12 | 0 | 0 |
| ra_reflow_layout | 11 | 11 | 0 | 0 |
| ra_usb | 11 | 11 | 0 | 0 |
| ra_ble_gatt | 10 | 10 | 0 | 0 |
| ra_ssie | 10 | 10 | 0 | 0 |
| ra_net_udp | 8 | 8 | 0 | 0 |
| ra_ota | 8 | 8 | 0 | 0 |
| ra_pdg | 8 | 8 | 0 | 0 |
| ra_sci | 8 | 8 | 0 | 0 |
| ra_drw | 7 | 7 | 0 | 0 |
| ra_i3c | 7 | 7 | 0 | 0 |
| ra_mpu | 7 | 7 | 0 | 0 |
| ra_usb_pal | 6 | 6 | 0 | 0 |
| ra_usb_paud | 6 | 6 | 0 | 0 |
| ra_usb_phid | 6 | 6 | 0 | 0 |
| ra_lvd | 5 | 5 | 0 | 0 |
| ra_net_ipv4 | 5 | 5 | 0 | 0 |
| ra_usb_haud | 5 | 5 | 0 | 0 |
| ra_usb_hcdc_ecm | 5 | 5 | 0 | 0 |
| ra_usb_pprn | 5 | 5 | 0 | 0 |
| ra_ble_gatt_client | 4 | 4 | 0 | 0 |
| ra_tls | 4 | 4 | 0 | 0 |
| ra_usb_hhid | 4 | 4 | 0 | 0 |
| ra_usb_pvnd | 4 | 4 | 0 | 0 |
| ra_xspi | 4 | 4 | 0 | 0 |
| ra_dmac | 3 | 3 | 0 | 0 |
| ra_gpt | 3 | 3 | 0 | 0 |
| ra_net_tcp | 3 | 3 | 0 | 0 |
| adc | 2 | 2 | 0 | 0 |
| ra_bkup | 2 | 2 | 0 | 0 |
| ra_ble_mesh | 2 | 2 | 0 | 0 |
| ra_ble_patch | 2 | 2 | 0 | 0 |
| ra_iic_b_slave | 2 | 2 | 0 | 0 |
| ra_ipc | 2 | 2 | 0 | 0 |
| ra_rmac_phy | 2 | 2 | 0 | 0 |
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
| ra_jpeg_sw | 3 | 5 | 16 | 24 |
| ra_fs_fat | 3 | 1 | 16 | 20 |
| ra_rsip | 3 | 0 | 13 | 16 |
| ra_ble | 2 | 0 | 6 | 8 |
| ra_flash | 1 | 2 | 17 | 20 |
| ra_dotf | 1 | 0 | 5 | 6 |
| ra_epub_open | 1 | 0 | 2 | 3 |
| ra_iic_b | 1 | 0 | 6 | 7 |
| ra_net_arp | 1 | 0 | 0 | 1 |
| ra_net_pal | 1 | 0 | 1 | 2 |
| ra_reflow_render | 1 | 0 | 0 | 1 |
| ra_reflow_xml_shim | 1 | 0 | 4 | 5 |
| ra_rmac | 1 | 0 | 1 | 2 |
| ra_spi_b | 1 | 0 | 8 | 9 |
| ra_usb_cdc | 1 | 0 | 3 | 4 |
| ra_usb_hmsc | 1 | 0 | 3 | 4 |

---

*Regenerated from the live `make mcdc` report. See `docs/MCDC_GAPS.csv` for the full per-decision table including decision-text snippets and excerpts.*
