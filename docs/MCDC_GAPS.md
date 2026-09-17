# MC/DC Coverage Gap Audit

Live audit of compound boolean decisions reported by `llvm-cov show --show-mcdc` for first-party sources (`libs/`, `port/`, excluding `libs/third_party/`). Regenerated from `build/mcdc-report/mcdc.txt` by `scripts/fix/regen_mcdc_gaps.py`; do not edit by hand.

## Methodology

- Source of truth: `build/mcdc-report/mcdc.txt` (output of `just quality::local::mcdc`).
- A decision is one llvm-cov "MC/DC Decision Region". Condition count is taken from the `Number of Conditions:` field that llvm-cov emits for that region.
- Coverage status (`covered` column):
  - `yes` -- llvm-cov reports 100.00% MC/DC for the decision. Excluded from the CSV (CSV is gap-only).
  - `partial` -- 0 < MC/DC % < 100. The decision was exercised but at least one independence pair is missing.
  - `no` -- MC/DC % == 0. The decision was never evaluated under instrumentation.

## Top-line Numbers

- Source files with at least one decision: **275**
- Total compound decisions in scope: **1567**
- Decisions at 100% MC/DC (`yes`): **1481**
- Decisions partially covered (`partial`): **64**
- Decisions fully uncovered (`no`): **22**
- Decision-complete rate (fully covered decisions / total decisions): **94.51%**
- Deactivated gap decision regions (DO-178C 6.4.4.3): **86**
- Reachable decision-region denominator (total - deactivated): **1481**
- **Reachable decision-complete MC/DC rate**: **100.00%** -- every counted decision region has complete MC/DC; the enforced ratchet threshold is recorded in `.github/mcdc-baseline.txt`.

See `docs/MCDC_DEACTIVATIONS.md` for the per-decision deactivation rationale catalog.

## Reachable gaps (require new MC/DC test vectors)

| File | Conds | Function | Excerpt | Status |
|------|------:|----------|---------|--------|

## Deactivated gaps (DO-178C 6.4.4.3 exempted)

These decision regions are unreachable on any public-API path and are therefore exempted from the reachable decision-complete gate. Each row carries the rationale used by the auto-classifier; humans may extend the per-decision narrative in `docs/MCDC_DEACTIVATIONS.md`.

| File | Conds | Function | Excerpt | Rationale |
|------|------:|----------|---------|-----------|
| apps/shared_libs/book/src/book_chunked_validate.c | 2 | internal_chunk_flat_read | `if ((cursor > ctx->rd->inflated_total) \|\| ((uint64_t)le...` | Annotated deactivation: internal_chunk_flat_read range gu... |
| apps/shared_libs/book/src/book_stream.c | 3 | internal_validate_raster | `if ((expect > (uint64_t)UINT32_MAX) \|\|` | Annotated deactivation: internal_validate_raster overflow... |
| apps/shared_libs/epub/src/epub_xml_shim.c | 2 | (file scope) | `if ((xml_attr_next(source, source_len, event, &cursor, &a...` | Annotated deactivation: priv_epub_xml_attr attribute re-w... |
| apps/shared_libs/epub/src/epub_xml_shim.c | 3 | priv_epub_xml_ancestor_marker | `return ((err == k_ra8_ok) && ((manifest_depth == UINT16_M...` | Annotated deactivation: internal_opf_status package-shape... |
| apps/shared_libs/epub/src/epub_xml_shim.c | 2 | priv_epub_xml_ancestor_marker | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_xml_...` | Annotated deactivation: internal_opf_first pull-loop stat... |
| apps/shared_libs/epub/src/epub_xml_shim.c | 2 | priv_epub_xml_ancestor_marker | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_xml_...` | Annotated deactivation: internal_collect_spine pull-loop ... |
| apps/shared_libs/epub/src/epub_xml_shim.c | 2 | priv_epub_xml_ancestor_marker | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_xml_...` | Annotated deactivation: internal_manifest_lookup pull-loo... |
| apps/shared_libs/epub/src/epub_xml_shim.c | 2 | priv_epub_xml_ancestor_marker | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_xml_...` | Annotated deactivation: internal_opf_shape pull-loop stat... |
| apps/shared_libs/epub/src/epub_xml_shim.c | 3 | priv_epub_xml_ancestor_marker | `if ((err == k_ra8_ok) && (!saw_manifest \|\| !saw_spine)) {` | Annotated deactivation: internal_opf_shape completeness g... |
| apps/shared_libs/epub/src/epub_xml_shim.c | 2 | priv_epub_xml_ancestor_marker | `for (uint16_t i = 0U; (err == k_ra8_ok) && (i < book->xml...` | Annotated deactivation: internal_opf_resolve_refs spine-r... |
| apps/shared_libs/epub/src/epub_xml_shim.c | 3 | priv_epub_xml_ancestor_marker | `if ((err == k_ra8_ok) && (book->cover_path[0] == '\0') &&` | Annotated deactivation: internal_opf_resolve_refs legacy-... |
| apps/shared_libs/epub/src/epub_xml_shim.c | 3 | priv_epub_xml_ancestor_marker | `if ((err == k_ra8_ok) && (book->toc_kind != (uint8_t)k_ep...` | Annotated deactivation: internal_opf_resolve_refs spine `...` |
| apps/shared_libs/epub/src/epub_xml_toc.c | 2 | internal_toc_marker | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_xml_...` | Annotated deactivation: internal_toc_capacity pull-loop s... |
| apps/shared_libs/epub/src/epub_xml_toc.c | 2 | priv_epub_xml_parse_ncx | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_xml_...` | Annotated deactivation: priv_epub_xml_parse_ncx pull-loop... |
| apps/shared_libs/epub/src/epub_xml_toc.c | 2 | priv_epub_xml_parse_ncx | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_xml_...` | Annotated deactivation: internal_select_nav pull-loop sta... |
| apps/shared_libs/epub/src/epub_xml_toc.c | 2 | priv_epub_xml_parse_ncx | `if ((err == k_ra8_ok) && (fallback.kind != (uint8_t)k_xml...` | Annotated deactivation: internal_select_nav fallback gate... |
| apps/shared_libs/epub/src/epub_xml_toc.c | 2 | priv_epub_xml_parse_ncx | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_xml_...` | Annotated deactivation: internal_nav_has_list pull-loop s... |
| apps/shared_libs/epub/src/epub_xml_toc.c | 2 | internal_nav_event | `} else if ((event->kind == (uint8_t)k_xml_event_text) && ...` | Annotated deactivation: internal_nav_event text-depth gat... |
| apps/shared_libs/epub/src/epub_xml_toc.c | 2 | priv_epub_xml_parse_nav | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_xml_...` | Annotated deactivation: priv_epub_xml_parse_nav pull-loop... |
| apps/shared_libs/epub/src/epub_xml_toc.c | 2 | priv_epub_xml_parse_nav | `if ((err != k_ra8_ok) \|\| !ctx.saw_ol) {` | Annotated deactivation: priv_epub_xml_parse_nav completio... |
| apps/shared_libs/jof/src/jof_png.c | 2 | internal_png_inflate_step | `if ((it->in_pos == it->in_avail) && (st->source_done == 0...` | Annotated deactivation: refill-first loop structure; ente... |
| apps/shared_libs/jof/src/jof_png.c | 2 | internal_png_inflate_step | `if ((in_sz == 0U) && (out_sz == 0U)) {` | Annotated deactivation: zero-progress stall guard, defens... |
| apps/shared_libs/jof/src/jof_png.c | 2 | internal_png_inflate_step | `if ((it->stalls > 1U) \|\| (st->source_done != 0U)) {` | Annotated deactivation: inner arm of the non-constructibl... |
| apps/shared_libs/jof/src/jof_produce.c | 2 | internal_carve_pixel_path | `if ((band_bytes > (uint64_t)UINT32_MAX) \|\| (stage_bytes...` | Annotated deactivation: stage_bytes = tw_eff*th_eff*bpp w... |
| apps/shared_libs/jof/src/jof_produce.c | 3 | internal_flush_band | `if ((nrows == 0U) \|\| ((uint32_t)y0 != st->rows_seen) \|\|` | Annotated deactivation: row-ordering contract guard; the ... |
| apps/shared_libs/jof/src/jof_produce.c | 2 | internal_epilogue | `if ((st->geom_done == 0U) \|\| (st->rows_seen != (uint32_...` | Annotated deactivation: post-decode contract guard; both ... |
| apps/shared_libs/longstrip/src/longstrip.c | 2 | longstrip_tick | `if ((wt->velocity == 0) \|\| internal_fling_should_stop(w...` | Pointer(s) ['wt'] already null-checked upstream in the sa... |
| apps/shared_libs/reflow/src/reflow_css.c | 3 | internal_ci_contains | `if ((s == nullptr) \|\| (sl == 0U) \|\| (sl > len)) {` | Annotated deactivation: the sole caller passes internal_c... |
| apps/shared_libs/reflow/src/reflow_css.c | 2 | internal_parse_color | `if ((s == nullptr) \|\| (len == 0U)) {` | Annotated deactivation: the sole caller internal_apply_de... |
| apps/shared_libs/reflow/src/reflow_css_cascade.c | 4 | internal_rule_rank | `if ((!have) \|\| (rank > best_rank) \|\|` | Annotated deactivation: rules[].order is assigned monoton... |
| apps/shared_libs/reflow/src/reflow_css_cascade.c | 3 | internal_ci_eq_span | `if ((a == nullptr) \|\| (b == nullptr) \|\| (alen != blen...` | Annotated deactivation: internal_ci_eq_span is reached on... |
| apps/shared_libs/reflow/src/reflow_css_rules.c | 2 | internal_intern_name | `if ((len == 0U) \|\| (len > (size_t)k_ra8_css_name_max)) {` | Annotated deactivation: every caller guards the length be... |
| apps/shared_libs/reflow/src/reflow_css_rules.c | 2 | internal_parse_sel_type | `while ((i < len) && priv_reflow_css_is_ws(s[i])) {` | Annotated deactivation: internal_split_compounds is only ... |
| apps/shared_libs/reflow/src/reflow_image.c | 2 | internal_decode_fail | `if ((reason != nullptr) && (strstr(reason, "outofmem") !=...` | Annotated deactivation: stbi sets a reason on every failu... |
| apps/shared_libs/reflow/src/reflow_image.c | 3 | internal_arena_release | `if ((pixels == nullptr) \|\| (sx <= 0) \|\| (sy <= 0)) {` | Annotated deactivation: stbi guarantees sx,sy &gt;= 1 when p... |
| apps/shared_libs/reflow/src/reflow_layout_image.c | 2 | internal_place_image | `if (((cur->y + (int32_t)cur->line_height_px) > bottom_lim...` | Annotated deactivation: internal_image_record incremented... |
| apps/shared_libs/reflow/src/reflow_layout_table.c | 2 | internal_table_columns | `if ((*cx > cell_x) && ((*cx + adv) <= cell_right)) {` | Annotated deactivation: priv_reflow_tok_stash_run collaps... |
| apps/shared_libs/reflow/src/reflow_render.c | 2 | internal_init_font | `if ((w > 0) && (h > 0)) {` | Annotated deactivation: glyph bbox w,h co-dependent (inke... |
| apps/shared_libs/reflow/src/reflow_svg_doc.c | 2 | internal_xform_with_group | `const bool self_close = (close > i) && (s[close - 1U] == ...` | Annotated deactivation: close&gt;i is invariant for a named ... |
| apps/shared_libs/reflow/src/reflow_svg_path.c | 2 | internal_next_cmd | `const bool    rel = (c >= 'a') && (c <= 'z');` | Annotated deactivation: c is a command letter or 0; (c&lt;='... |
| apps/shared_libs/reflow/src/reflow_svg_shape.c | 2 | internal_sort_i32 | `for (int32_t i = 0; (i < n) && (m < (int32_t)k_svg_poly_m...` | Annotated deactivation: the polygon point count n is itse... |
| apps/shared_libs/reflow/src/reflow_svg_shape.c | 4 | internal_sort_i32 | `if (((y0 <= y) && (y < y1)) \|\| ((y1 <= y) && (y < y0))) {` | Annotated deactivation: the fourth condition (y &lt; y0) is ... |
| apps/shared_libs/reflow/src/reflow_svg_shape.c | 2 | internal_grad_eval | `if ((p >= o0) && (p <= o1)) {` | Annotated deactivation: the loop is entered only when p &gt;... |
| apps/shared_libs/reflow/src/reflow_tokenize_lex.c | 3 | priv_reflow_tok_classify | `if ((i < avail) && ((src[i] == 'x') \|\| (src[i] == 'X'))) {` | Annotated deactivation: the sole caller priv_reflow_tok_d... |
| apps/shared_libs/reflow/src/reflow_tokenize_lex.c | 3 | priv_reflow_tok_classify | `if ((digits == 0U) \|\| (i >= avail) \|\| (src[i] != ';')) {` | Annotated deactivation: the scan loop above exits with i ... |
| apps/shared_libs/xml/src/xml.c | 3 | xml_attr_begin | `if ((err != k_ra8_ok) \|\| (count == UINT16_MAX) \|\|` | Annotated deactivation: internal_attributes attribute-cou... |
| apps/shared_libs/xml/src/xml.c | 2 | xml_attr_begin | `if ((cursor.position >= end) \|\| (source[cursor.position...` | Annotated deactivation: internal_attributes self-closing ... |
| apps/shared_libs/xml/src/xml.c | 5 | internal_encoding | `if (!initial \|\| (reader->declaration_seen != 0U) \|\| (...` | Annotated deactivation: internal_declaration placement ga... |
| apps/shared_libs/xml/src/xml.c | 2 | internal_cdata | `if (((pos + 2U) <= reader->source_len) && (reader->source...` | Annotated deactivation: internal_special processing-instr... |
| apps/shared_libs/xml/src/xml.c | 3 | xml_reader_next | `if ((reader->stack_size != 0U) \|\| (reader->root_count !...` | Annotated deactivation: xml_reader_next end-of-document c... |
| apps/shared_libs/xml/src/xml_decode.c | 3 | internal_digit | `if ((cursor < end) && ((source[cursor] == (uint8_t)'x') \...` | Annotated deactivation: internal_entity radix probe; the ... |
| apps/shared_libs/xml/src/xml_decode.c | 3 | xml_decoded_equal | `if ((internal_decoded_byte(source, &li, &lb) != k_ra8_ok)...` | Annotated deactivation: xml_decoded_equal byte-walk statu... |
| apps/shared_libs/xml/src/xml_doctype.c | 2 | internal_pubid_byte | `if (((*position + length) > end) \|\| !priv_xml_bytes_equ...` | Annotated deactivation: internal_keyword prefix recheck; ... |
| libs/ra8_app/src/ra8_app.c | 2 | (file scope) | `if ((next != nullptr) && (next->vt->on_enter != nullptr)) {` | Annotated deactivation: next=reg-&gt;apps[target] with targe... |
| libs/ra8_box/src/ra8_box.c | 2 | internal_iter_live | `return (link != (int32_t)k_ra8_box_none) && (guard < count);` | Annotated deactivation: guard&lt;count is an acyclic-tree cy... |
| libs/ra8_c6link/src/ra8_c6link_mdl.c | 2 | internal_mdl_take_response | `if ((body->data.data == nullptr) \|\| (body->data.len == ...` | Annotated deactivation: internal_mdl_take_response empty-... |
| libs/ra8_c6link/src/ra8_c6link_mdl.c | 3 | ra8_c6link_mdl_chunk_semantics_valid_test | `if ((packed == 0U) \|\| (packed > sizeof(link->mdl_reques...` | Annotated deactivation: ra8_c6link_mdl_start_request code... |
| libs/ra8_c6link/src/ra8_c6link_mdl.c | 3 | ra8_c6link_mdl_chunk_semantics_valid_test | `if ((packed == 0U) \|\| (packed > sizeof(link->mdl_reques...` | Annotated deactivation: ra8_c6link_mdl_next codec self-co... |
| libs/ra8_c6link/src/ra8_c6link_mdl.c | 3 | ra8_c6link_mdl_cancel | `if ((packed == 0U) \|\| (packed > sizeof(link->mdl_reques...` | Annotated deactivation: ra8_c6link_mdl_cancel codec self-... |
| libs/ra8_c6link/src/ra8_c6link_mdl_transfer.c | 2 | (file scope) | `} else if ((err == k_ra8_ok) && (chunk.state == k_ra8_mdl...` | Annotated deactivation: ra8_c6link_mdl_transfer CANCELLED... |
| libs/ra8_c6link/src/ra8_c6link_mdl_transfer.c | 2 | (file scope) | `if ((err == k_ra8_ok) && state.session.active) {` | Annotated deactivation: ra8_c6link_mdl_transfer exhausted... |
| libs/ra8_core/src/ra8_log.c | 2 | internal_itm_put_u32 | `while (value != 0U && i < k_ra8_u32_max_digits) {` | Annotated deactivation: internal_itm_put_u32 digit-buffer... |
| libs/ra8_dfu/src/ra8_rot.c | 2 | internal_ct_equal | `if ((psa_err != k_ra8_ok) && (psa_err != k_ra8_err_exists...` | Annotated deactivation: DO-178C 6.4.4.3 -- under RA8_OFF_... |
| libs/ra8_hal/src/ra8_ble.c | 2 | internal_dispatch_event | `if ((internal_rx_byte(&code) == 0U) \|\| (internal_rx_byt...` | Annotated deactivation: TU-local helper internal_dispatch... |
| libs/ra8_hal/src/ra8_ble.c | 4 | internal_dispatch_acl | `if ((internal_rx_byte(&hdl_lo) == 0U) \|\| (internal_rx_b...` | Annotated deactivation: TU-local helper internal_dispatch... |
| libs/ra8_hal/src/ra8_canfd_timing.c | 2 | (file scope) | `if ((bitrate_bps == 0U) \|\| (clock_hz == 0U)) {` | Annotated deactivation: both args are validated by ra8_ca... |
| libs/ra8_hal/src/ra8_ceu.c | 2 | internal_arm_capture | `if ((s_ceu_image_area != 0U) && (bufs->y_top != nullptr)) {` | Annotated deactivation: TU-local helper internal_arm_capt... |
| libs/ra8_hal/src/ra8_dotf.c | 2 | internal_check_overlap | `if ((region->start_addr <= live->end_addr) && (live->star...` | Annotated deactivation: ra8_dotf overlap-detection AND; t... |
| libs/ra8_hal/src/ra8_i3c_i2c.c | 2 | internal_i3c_i2c_half_period | `if ((bus_hz == 0U) \|\| (pclka_hz == 0U)) {` | Annotated deactivation: both args validated by ra8_i3c_i2... |
| libs/ra8_hal/src/ra8_mipi_dsi_dispatch.c | 2 | ra8_mipi_dsi_dispatch_receive | `if ((s_mipi_dsi_pending_rx_buffer != nullptr) && (s_mipi_...` | Annotated deactivation: ra8_mipi_dsi_dispatch_receive pen... |
| libs/ra8_hal/src/ra8_mipi_phy_timing.c | 3 | (file scope) | `if ((tbl[i].mode == mode_flag) && (tbl[i].pclka == pclka)...` | Annotated deactivation: internal_mipi_phy_lookup_timing 3... |
| libs/ra8_hal/src/ra8_rmac_mgmt.c | 2 | ra8_rmac_phy_auto_neg_start | `if (out_link->up && ((bmsr & (uint16_t)k_ra8_rmac_phy_bms...` | Annotated deactivation: ra8_rmac_phy_link_status link-up ... |
| libs/ra8_hal/src/ra8_spi_b.c | 2 | internal_unit_bytes | `if ((tx == nullptr) && (rx == nullptr)) {` | Annotated deactivation: TU-local helper internal_xfer_com... |
| libs/ra8_hal/src/ra8_vin.c | 2 | internal_mc_rmw | `if (((mc_now & k_ra8_vin_mc_me) != 0UL) \|\| ((fc_now & k...` | Annotated deactivation: ra8_vin idle-state guard; MC.ME (... |
| libs/ra8_jpeg/src/ra8_jpeg_sw_decode.c | 2 | priv_jpeg_sw_parse_dqt | `if (len < 2U \|\| (uint32_t)len > d->src_len - d->cursor) {` | Defensive segment-length bound in a bounded parser: buffe... |
| libs/ra8_jpeg/src/ra8_jpeg_sw_decode.c | 2 | priv_jpeg_sw_parse_sof0 | `if (len < 8U \|\| (uint32_t)len > d->src_len - d->cursor) {` | Defensive segment-length bound in a bounded parser: buffe... |
| libs/ra8_jpeg/src/ra8_jpeg_sw_decode.c | 2 | priv_jpeg_sw_parse_sos | `if (len < 6U \|\| (uint32_t)len > d->src_len - d->cursor) {` | Defensive segment-length bound in a bounded parser: buffe... |
| libs/ra8_jpeg/src/ra8_jpeg_sw_decode.c | 2 | priv_jpeg_sw_parse_sos | `if (r < 0 && t != 0) {` | Annotated deactivation: t == 0 requests no bits, so r &lt; 0... |
| libs/ra8_net_pal/src/ra8_net_pal.c | 2 | internal_eth_event | `if ((s_state.event_fn != nullptr) && (pal_mask != k_ra8_n...` | Annotated deactivation: TU-local helper internal_eth_even... |
| libs/ra8_psa_crypto/src/ra8_psa_crypto_fake.c | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < key_len) && (off < sizeof(buf));...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra8_psa_crypto/src/ra8_psa_crypto_fake.c | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < nonce_len) && (off < sizeof(buf)...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra8_psa_crypto/src/ra8_psa_crypto_fake.c | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < aad_len) && (off < sizeof(buf));...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra8_psa_crypto/src/ra8_psa_crypto_fake.c | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < key_len) && (off < sizeof(seed))...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra8_psa_crypto/src/ra8_psa_crypto_fake.c | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < nonce_len) && (off < sizeof(seed...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra8_touch_cal/src/ra8_touch_cal.c | 2 | internal_clip32 | `if (!ok_u \|\| !ok_v) {` | Annotated deactivation: ra8_touch_cal_compute internal_so... |
| libs/ra8_wdt_supervisor/src/ra8_wdt_supervisor.c | 2 | ra8_wdt_supervisor_tick | `if (will_refresh && (s_state.refresh != nullptr)) {` | Annotated deactivation: ra8_wdt_supervisor_tick refresh d... |

## Per-module gap counts (full table)

Sorted by (uncovered + partial) descending, then total descending.

| Module | Total | Covered | Partial | Uncovered |
|--------|------:|--------:|--------:|----------:|
| epub_xml_shim | 38 | 28 | 8 | 2 |
| epub_xml_toc | 32 | 24 | 5 | 3 |
| xml | 55 | 50 | 4 | 1 |
| ra8_psa_crypto_fake | 6 | 1 | 5 | 0 |
| ra8_c6link_mdl | 25 | 21 | 1 | 3 |
| ra8_jpeg_sw_decode | 16 | 12 | 4 | 0 |
| jof_produce | 19 | 16 | 2 | 1 |
| reflow_svg_shape | 15 | 12 | 3 | 0 |
| jof_png | 7 | 4 | 1 | 2 |
| reflow_css | 26 | 24 | 1 | 1 |
| reflow_css_cascade | 25 | 23 | 2 | 0 |
| xml_decode | 25 | 23 | 2 | 0 |
| reflow_css_rules | 21 | 19 | 2 | 0 |
| reflow_tokenize_lex | 12 | 10 | 2 | 0 |
| ra8_c6link_mdl_transfer | 10 | 8 | 2 | 0 |
| ra8_ble | 8 | 6 | 0 | 2 |
| reflow_image | 3 | 1 | 2 | 0 |
| reflow_svg_doc | 20 | 19 | 1 | 0 |
| book_stream | 16 | 15 | 1 | 0 |
| ra8_vin | 14 | 13 | 1 | 0 |
| ra8_touch_cal | 13 | 12 | 1 | 0 |
| xml_doctype | 13 | 12 | 0 | 1 |
| reflow_svg_path | 9 | 8 | 1 | 0 |
| ra8_i3c_i2c | 7 | 6 | 0 | 1 |
| ra8_spi_b | 7 | 6 | 0 | 1 |
| ra8_app | 6 | 5 | 1 | 0 |
| ra8_dotf | 6 | 5 | 0 | 1 |
| ra8_mipi_dsi_dispatch | 6 | 5 | 1 | 0 |
| ra8_rot | 6 | 5 | 0 | 1 |
| reflow_layout_image | 6 | 5 | 1 | 0 |
| reflow_layout_table | 6 | 5 | 1 | 0 |
| longstrip | 5 | 4 | 1 | 0 |
| book_chunked_validate | 4 | 3 | 0 | 1 |
| ra8_box | 4 | 3 | 1 | 0 |
| reflow_render | 4 | 3 | 1 | 0 |
| ra8_ceu | 3 | 2 | 1 | 0 |
| ra8_mipi_phy_timing | 3 | 2 | 1 | 0 |
| ra8_canfd_timing | 2 | 1 | 1 | 0 |
| ra8_net_pal | 2 | 1 | 0 | 1 |
| ra8_rmac_mgmt | 2 | 1 | 1 | 0 |
| ra8_wdt_supervisor | 2 | 1 | 1 | 0 |
| ra8_log | 1 | 0 | 1 | 0 |
| fw_if_fs | 31 | 31 | 0 | 0 |
| reflow_tokenize | 23 | 23 | 0 | 0 |
| epub_chapter | 21 | 21 | 0 | 0 |
| ra8_usb_haud | 20 | 20 | 0 | 0 |
| ra8_usb_hcdc_ecm | 20 | 20 | 0 | 0 |
| reflow_svg | 20 | 20 | 0 | 0 |
| ra8_mipi_phy | 19 | 19 | 0 | 0 |
| ra8_usb_cdc | 16 | 16 | 0 | 0 |
| ra8_usb_hhid | 16 | 16 | 0 | 0 |
| ra8_psa_crypto | 15 | 15 | 0 | 0 |
| ra8_c6link | 14 | 14 | 0 | 0 |
| ra8_etha | 13 | 13 | 0 | 0 |
| ra8_fs_fat_name | 13 | 13 | 0 | 0 |
| book_xhtml | 12 | 12 | 0 | 0 |
| ra8_esp_hosted_rtos_pool | 12 | 12 | 0 | 0 |
| ra8_gfx_text | 12 | 12 | 0 | 0 |
| ra8_modem_at | 12 | 12 | 0 | 0 |
| ra8_usb_hmsc | 12 | 12 | 0 | 0 |
| reflow_layout_driver | 12 | 12 | 0 | 0 |
| reflow_svg_xform | 11 | 11 | 0 | 0 |
| reflow_tokenize_attr | 11 | 11 | 0 | 0 |
| book_paged | 10 | 10 | 0 | 0 |
| ra8_esp_hosted_rtos | 10 | 10 | 0 | 0 |
| ra8_mipi_dsi_cmd | 10 | 10 | 0 | 0 |
| zoom | 10 | 10 | 0 | 0 |
| epub_open | 9 | 9 | 0 | 0 |
| mdl_service | 9 | 9 | 0 | 0 |
| ra8_esp_hosted_osi_absent | 9 | 9 | 0 | 0 |
| ra8_flash_irq | 9 | 9 | 0 | 0 |
| ra8_fs_fat_mount | 9 | 9 | 0 | 0 |
| mg_reader | 8 | 8 | 0 | 0 |
| ra8_c6link_mdl_service | 8 | 8 | 0 | 0 |
| ra8_epaper_geom | 8 | 8 | 0 | 0 |
| ra8_net_provision | 8 | 8 | 0 | 0 |
| ra8_pdg | 8 | 8 | 0 | 0 |
| ra8_rabook_comic | 8 | 8 | 0 | 0 |
| ra8_rsip_asym | 8 | 8 | 0 | 0 |
| ra8_usb_hcdc | 8 | 8 | 0 | 0 |
| ra8_usb_xfer | 8 | 8 | 0 | 0 |
| ra8_widget | 8 | 8 | 0 | 0 |
| reflow_layout | 8 | 8 | 0 | 0 |
| ra8_c6link_wifi_sta | 7 | 7 | 0 | 0 |
| ra8_esp_hosted_fmt | 7 | 7 | 0 | 0 |
| ra8_flash | 7 | 7 | 0 | 0 |
| ra8_fs_fat_check | 7 | 7 | 0 | 0 |
| ra8_fs_fat_fileio | 7 | 7 | 0 | 0 |
| ra8_i3c | 7 | 7 | 0 | 0 |
| ra8_io_stream_posix | 7 | 7 | 0 | 0 |
| ra8_mdl_storage_ram | 7 | 7 | 0 | 0 |
| ra8_mpu | 7 | 7 | 0 | 0 |
| ra8_ota_parse | 7 | 7 | 0 | 0 |
| ra8_sdmmc_spi_io | 7 | 7 | 0 | 0 |
| ra8_tls | 7 | 7 | 0 | 0 |
| book_stream_wire | 6 | 6 | 0 | 0 |
| epub_miniz_alloc | 6 | 6 | 0 | 0 |
| ez_scene | 6 | 6 | 0 | 0 |
| fw_if_fs_dir | 6 | 6 | 0 | 0 |
| ra8_dfu_boot | 6 | 6 | 0 | 0 |
| ra8_fs_fat_fmt | 6 | 6 | 0 | 0 |
| ra8_fs_fat_lfn | 6 | 6 | 0 | 0 |
| ra8_i2c | 6 | 6 | 0 | 0 |
| ra8_jpeg_sw | 6 | 6 | 0 | 0 |
| ra8_mipi_dsi | 6 | 6 | 0 | 0 |
| ra8_sci | 6 | 6 | 0 | 0 |
| ra8_ssie | 6 | 6 | 0 | 0 |
| ra8_usb_pal | 6 | 6 | 0 | 0 |
| ra8_usb_paud | 6 | 6 | 0 | 0 |
| ra8_usb_phid | 6 | 6 | 0 | 0 |
| unarch_tar_fields | 6 | 6 | 0 | 0 |
| jof_png_chunk | 5 | 5 | 0 | 0 |
| ra8_c6link_tlv | 5 | 5 | 0 | 0 |
| ra8_drw_draw | 5 | 5 | 0 | 0 |
| ra8_fs_fat_label | 5 | 5 | 0 | 0 |
| ra8_fs_utf | 5 | 5 | 0 | 0 |
| ra8_gpt | 5 | 5 | 0 | 0 |
| ra8_i2c_peripheral | 5 | 5 | 0 | 0 |
| ra8_io_vfs_namespace | 5 | 5 | 0 | 0 |
| ra8_rar5 | 5 | 5 | 0 | 0 |
| ra8_rar5_tables | 5 | 5 | 0 | 0 |
| ra8_usb_pprn | 5 | 5 | 0 | 0 |
| sec_cmac | 5 | 5 | 0 | 0 |
| zoom_tiles | 5 | 5 | 0 | 0 |
| ra8_batt | 4 | 4 | 0 | 0 |
| ra8_c6link_frame | 4 | 4 | 0 | 0 |
| ra8_c6link_rpc | 4 | 4 | 0 | 0 |
| ra8_dfu_program | 4 | 4 | 0 | 0 |
| ra8_drw | 4 | 4 | 0 | 0 |
| ra8_epaper | 4 | 4 | 0 | 0 |
| ra8_epd_cal | 4 | 4 | 0 | 0 |
| ra8_flash_config | 4 | 4 | 0 | 0 |
| ra8_fs_fat_exfat_check | 4 | 4 | 0 | 0 |
| ra8_fs_fat_lfn_write | 4 | 4 | 0 | 0 |
| ra8_gfx_text_glyph | 4 | 4 | 0 | 0 |
| ra8_lvd | 4 | 4 | 0 | 0 |
| ra8_rabook_gray4 | 4 | 4 | 0 | 0 |
| ra8_rsip | 4 | 4 | 0 | 0 |
| ra8_rsip_cipher | 4 | 4 | 0 | 0 |
| ra8_ssie_stream | 4 | 4 | 0 | 0 |
| ra8_usb_device | 4 | 4 | 0 | 0 |
| ra8_usb_pvnd | 4 | 4 | 0 | 0 |
| rabook_import | 4 | 4 | 0 | 0 |
| reflow_cache | 4 | 4 | 0 | 0 |
| adc_selfdiag | 3 | 3 | 0 | 0 |
| book | 3 | 3 | 0 | 0 |
| epub_fs | 3 | 3 | 0 | 0 |
| jof | 3 | 3 | 0 | 0 |
| ra8_c6link_arena | 3 | 3 | 0 | 0 |
| ra8_dmac | 3 | 3 | 0 | 0 |
| ra8_esp_hosted_log | 3 | 3 | 0 | 0 |
| ra8_esp_hosted_port | 3 | 3 | 0 | 0 |
| ra8_esp_hosted_rtos_sync | 3 | 3 | 0 | 0 |
| ra8_esp_hosted_spi | 3 | 3 | 0 | 0 |
| ra8_eth_gwca_queue | 3 | 3 | 0 | 0 |
| ra8_fs_fat_dir | 3 | 3 | 0 | 0 |
| ra8_fs_fat_exfat_stream | 3 | 3 | 0 | 0 |
| ra8_fs_fat_file | 3 | 3 | 0 | 0 |
| ra8_jpeg_sw_stream | 3 | 3 | 0 | 0 |
| ra8_ov5640 | 3 | 3 | 0 | 0 |
| ra8_sdmmc_spi | 3 | 3 | 0 | 0 |
| ra8_touch | 3 | 3 | 0 | 0 |
| ra8_ui | 3 | 3 | 0 | 0 |
| ra8_widget_keyboard | 3 | 3 | 0 | 0 |
| ra8_widget_panel | 3 | 3 | 0 | 0 |
| reflow_link | 3 | 3 | 0 | 0 |
| sh_classify | 3 | 3 | 0 | 0 |
| usb_printer_vendor_ch9 | 3 | 3 | 0 | 0 |
| adc | 2 | 2 | 0 | 0 |
| comic_wrapped | 2 | 2 | 0 | 0 |
| epub_img_import | 2 | 2 | 0 | 0 |
| jof_produce_webp | 2 | 2 | 0 | 0 |
| mkbookimg_names | 2 | 2 | 0 | 0 |
| ra8_agt | 2 | 2 | 0 | 0 |
| ra8_bkup_tamper | 2 | 2 | 0 | 0 |
| ra8_board_ek_ra8d2_audio_usb | 2 | 2 | 0 | 0 |
| ra8_devcfg | 2 | 2 | 0 | 0 |
| ra8_display_pal_policy | 2 | 2 | 0 | 0 |
| ra8_epaper_devinfo | 2 | 2 | 0 | 0 |
| ra8_esp_hosted_gpio | 2 | 2 | 0 | 0 |
| ra8_esp_hosted_gpio_edge | 2 | 2 | 0 | 0 |
| ra8_esp_hosted_osi | 2 | 2 | 0 | 0 |
| ra8_eth | 2 | 2 | 0 | 0 |
| ra8_fs_fat_attr | 2 | 2 | 0 | 0 |
| ra8_gfx_dither | 2 | 2 | 0 | 0 |
| ra8_i3c_i2c_peripheral | 2 | 2 | 0 | 0 |
| ra8_io_stream | 2 | 2 | 0 | 0 |
| ra8_ipc | 2 | 2 | 0 | 0 |
| ra8_jpeg_sw_encode | 2 | 2 | 0 | 0 |
| ra8_keyboard | 2 | 2 | 0 | 0 |
| ra8_rabook_pipeline | 2 | 2 | 0 | 0 |
| ra8_rabook_xml_shim | 2 | 2 | 0 | 0 |
| ra8_rmac_phy | 2 | 2 | 0 | 0 |
| ra8_rsip_ecc | 2 | 2 | 0 | 0 |
| ra8_rsip_rsa | 2 | 2 | 0 | 0 |
| ra8_sdhi | 2 | 2 | 0 | 0 |
| ra8_tile_cache | 2 | 2 | 0 | 0 |
| ra8_tsn | 2 | 2 | 0 | 0 |
| ra8_usb_composite | 2 | 2 | 0 | 0 |
| ra8_usb_hhub | 2 | 2 | 0 | 0 |
| ra8_usb_host_ctrl | 2 | 2 | 0 | 0 |
| ra8_usb_irq | 2 | 2 | 0 | 0 |
| ra8_vreg | 2 | 2 | 0 | 0 |
| ra8_wdt | 2 | 2 | 0 | 0 |
| ra8_webp | 2 | 2 | 0 | 0 |
| ra8_webp_arena | 2 | 2 | 0 | 0 |
| ra8_widget_book | 2 | 2 | 0 | 0 |
| ra8_widget_nav_bar | 2 | 2 | 0 | 0 |
| ra8_widget_reflow_view | 2 | 2 | 0 | 0 |
| ra8_xspi | 2 | 2 | 0 | 0 |
| ra8_xspi_flash | 2 | 2 | 0 | 0 |
| secure_trng | 2 | 2 | 0 | 0 |
| unarch_tar | 2 | 2 | 0 | 0 |
| board_periph_mstp_model | 1 | 1 | 0 | 0 |
| comic | 1 | 1 | 0 | 0 |
| comic_cbr | 1 | 1 | 0 | 0 |
| comic_tiles | 1 | 1 | 0 | 0 |
| epub_img_tiles | 1 | 1 | 0 | 0 |
| key_import | 1 | 1 | 0 | 0 |
| key_vault | 1 | 1 | 0 | 0 |
| ota_commit | 1 | 1 | 0 | 0 |
| ra8_board_ek_ra8d2_camera | 1 | 1 | 0 | 0 |
| ra8_c6link_pump | 1 | 1 | 0 | 0 |
| ra8_cache | 1 | 1 | 0 | 0 |
| ra8_canfd_frame | 1 | 1 | 0 | 0 |
| ra8_cgc_eswclk | 1 | 1 | 0 | 0 |
| ra8_crashlog | 1 | 1 | 0 | 0 |
| ra8_crc | 1 | 1 | 0 | 0 |
| ra8_display_pal_eink | 1 | 1 | 0 | 0 |
| ra8_display_pal_lcd | 1 | 1 | 0 | 0 |
| ra8_dual_core | 1 | 1 | 0 | 0 |
| ra8_eth_gwca | 1 | 1 | 0 | 0 |
| ra8_eth_gwca_default | 1 | 1 | 0 | 0 |
| ra8_eth_mfwd | 1 | 1 | 0 | 0 |
| ra8_etha_stats | 1 | 1 | 0 | 0 |
| ra8_ether_phy | 1 | 1 | 0 | 0 |
| ra8_ethosu_shim | 1 | 1 | 0 | 0 |
| ra8_fs_fat_alloc | 1 | 1 | 0 | 0 |
| ra8_fs_fat_exfat_fmt | 1 | 1 | 0 | 0 |
| ra8_fs_fat_exfat_label | 1 | 1 | 0 | 0 |
| ra8_fs_fat_exfat_mutate | 1 | 1 | 0 | 0 |
| ra8_fs_fat_exfat_openw | 1 | 1 | 0 | 0 |
| ra8_fs_fat_exfat_read | 1 | 1 | 0 | 0 |
| ra8_fs_fat_lock | 1 | 1 | 0 | 0 |
| ra8_fs_fat_space | 1 | 1 | 0 | 0 |
| ra8_fs_fat_truncate | 1 | 1 | 0 | 0 |
| ra8_fs_fat_utime | 1 | 1 | 0 | 0 |
| ra8_gfx_blit_gray4 | 1 | 1 | 0 | 0 |
| ra8_glcdc_layer | 1 | 1 | 0 | 0 |
| ra8_img_arena | 1 | 1 | 0 | 0 |
| ra8_io_blockdev | 1 | 1 | 0 | 0 |
| ra8_io_vfs | 1 | 1 | 0 | 0 |
| ra8_isr | 1 | 1 | 0 | 0 |
| ra8_lvd_runtime | 1 | 1 | 0 | 0 |
| ra8_mipi_csi | 1 | 1 | 0 | 0 |
| ra8_nsc_eth | 1 | 1 | 0 | 0 |
| ra8_nsc_ota | 1 | 1 | 0 | 0 |
| ra8_nsc_xspi | 1 | 1 | 0 | 0 |
| ra8_ota | 1 | 1 | 0 | 0 |
| ra8_pwr | 1 | 1 | 0 | 0 |
| ra8_rar | 1 | 1 | 0 | 0 |
| ra8_rtc | 1 | 1 | 0 | 0 |
| ra8_sci_lin | 1 | 1 | 0 | 0 |
| ra8_smbus | 1 | 1 | 0 | 0 |
| ra8_sram | 1 | 1 | 0 | 0 |
| ra8_systick | 1 | 1 | 0 | 0 |
| ra8_tz_secure_boot | 1 | 1 | 0 | 0 |
| ra8_usb_pmsc | 1 | 1 | 0 | 0 |
| rabook_compile | 1 | 1 | 0 | 0 |
| rabook_import_compiler | 1 | 1 | 0 | 0 |
| reflow_parse | 1 | 1 | 0 | 0 |
| tx_systick_retune | 1 | 1 | 0 | 0 |
| wifi_hal_core | 1 | 1 | 0 | 0 |
| zoom_book | 1 | 1 | 0 | 0 |
| zoom_render | 1 | 1 | 0 | 0 |

## Top 30 modules with at least one uncovered decision

| Module | Uncovered | Partial | Covered | Total |
|--------|----------:|--------:|--------:|------:|
| epub_xml_toc | 3 | 5 | 24 | 32 |
| ra8_c6link_mdl | 3 | 1 | 21 | 25 |
| epub_xml_shim | 2 | 8 | 28 | 38 |
| jof_png | 2 | 1 | 4 | 7 |
| ra8_ble | 2 | 0 | 6 | 8 |
| xml | 1 | 4 | 50 | 55 |
| jof_produce | 1 | 2 | 16 | 19 |
| reflow_css | 1 | 1 | 24 | 26 |
| book_chunked_validate | 1 | 0 | 3 | 4 |
| ra8_dotf | 1 | 0 | 5 | 6 |
| ra8_i3c_i2c | 1 | 0 | 6 | 7 |
| ra8_net_pal | 1 | 0 | 1 | 2 |
| ra8_rot | 1 | 0 | 5 | 6 |
| ra8_spi_b | 1 | 0 | 6 | 7 |
| xml_doctype | 1 | 0 | 12 | 13 |

---

*Regenerated from the live `just quality::local::mcdc` report. See `docs/MCDC_GAPS.csv` for the full per-decision table including decision-text snippets and excerpts.*
