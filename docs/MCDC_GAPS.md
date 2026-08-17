# MC/DC Coverage Gap Audit

Live audit of compound boolean decisions reported by `llvm-cov show --show-mcdc` for first-party sources (`libs/`, `src/`, `port/`, excluding `libs/third_party/`). Regenerated from `build/mcdc-report/mcdc.txt` by `scripts/fix/regen_mcdc_gaps.py`; do not edit by hand.

## Methodology

- Source of truth: `build/mcdc-report/mcdc.txt` (output of `make mcdc`).
- A decision is one llvm-cov "MC/DC Decision Region". Condition count is taken from the `Number of Conditions:` field that llvm-cov emits for that region.
- Coverage status (`covered` column):
  - `yes` -- llvm-cov reports 100.00% MC/DC for the decision. Excluded from the CSV (CSV is gap-only).
  - `partial` -- 0 < MC/DC % < 100. The decision was exercised but at least one independence pair is missing.
  - `no` -- MC/DC % == 0. The decision was never evaluated under instrumentation.

## Top-line Numbers

- Source files with at least one decision: **276**
- Total compound decisions in scope: **1577**
- Decisions at 100% MC/DC (`yes`): **1375**
- Decisions partially covered (`partial`): **90**
- Decisions fully uncovered (`no`): **112**
- Coverage rate (yes / total): **87.19%**
- Deactivated gap conditions (DO-178C 6.4.4.3): **99**
- Reachable-condition denominator (total - deactivated): **1478**
- **Reachable MC/DC rate**: **93.03%** -- the enforced ratchet threshold is recorded in `.github/mcdc-baseline.txt`.

See `docs/MCDC_DEACTIVATIONS.md` for the per-condition deactivation rationale catalog.

## Reachable gaps (require new MC/DC test vectors)

| File | Conds | Function | Excerpt | Status |
|------|------:|----------|---------|--------|
| examples/ek_ra8d2/hw_pending/ereader_manga/src/mg_reader.c | 2 | mg_offsets | `if (((rx % s) != 0) \|\| ((ry % s) != 0)) {` | no |
| examples/ek_ra8d2/hw_pending/ereader_manga/src/mg_reader.c | 2 | mg_offsets | `if ((px < 0) \|\| (px >= r->fb_w)) {` | no |
| examples/ek_ra8d2/hw_pending/ereader_manga/src/mg_reader.c | 2 | mg_offsets | `if ((py < (int32_t)k_mg_statusbar_h) \|\| (py >= r->fb_h)) {` | no |
| examples/ek_ra8d2/hw_pending/ereader_manga/src/mg_reader.c | 2 | mg_clamp | `if ((sy < reg[1]) \|\| (sy >= reg[3])) {` | partial |
| examples/ek_ra8d2/hw_pending/ereader_manga/src/mg_reader.c | 2 | mg_clamp | `if ((sx < reg[0]) \|\| (sx >= reg[2])) {` | no |
| examples/ek_ra8d2/hw_pending/ereader_manga/src/mg_reader.c | 2 | mg_append_uint | `while ((v > 0U) && (n < (uint32_t)k_mg_dec_max)) {` | partial |
| examples/ek_ra8d2/hw_pending/ereader_manga/src/mg_reader.c | 2 | mg_append_uint | `for (uint32_t i = 0U; (i < n) && (pos < (cap - 1U)); ++i) {` | partial |
| examples/ek_ra8d2/hw_pending/ereader_manga/src/mg_reader.c | 3 | mg_append_str | `for (uint32_t i = 0U; (i < (uint32_t)k_mg_str_max) && (s[i] != '\0') && (pos ...` | partial |
| examples/ek_ra8d2/hw_pending/ereader_manga/src/mg_reader.c | 2 | mg_reader_check_geometry | `if ((cfg->fb_w <= 0) \|\| (cfg->fb_h <= (int32_t)k_mg_statusbar_h)) {` | no |
| examples/ek_ra8d2/hw_pending/ereader_manga/src/mg_reader.c | 4 | mg_reader_check_geometry | `if ((cfg->info->width == 0U) \|\| (cfg->info->height == 0U) \|\| (cfg->info->...` | no |
| examples/ek_ra8d2/hw_pending/ereader_zoom/src/ez_scene.c | 2 | ez_fill_tile | `if ((org_x >= (uint32_t)k_ez_page_w) \|\| (org_y >= (uint32_t)k_ez_page_h)) {` | partial |
| examples/ek_ra8d2/hw_pending/ereader_zoom/src/ez_scene.c | 2 | ez_scene_init | `if ((cfg->fb_w <= (int32_t)k_ez_lens_edge) \|\|` | partial |
| examples/ek_ra8d2/hw_pending/ereader_zoom/src/ez_scene.c | 2 | ez_scene_render | `if (lens_plan->present && s->lens_on) {` | partial |
| examples/ek_ra8d2/hw_pending/ereader_zoom/src/ez_scene.c | 3 | ez_scene_tick | `return page_due \|\| (lens_due && s->lens_on);` | partial |
| examples/ek_ra8d2/hw_pending/media_download/media_download_format.c | 3 | (file scope) | `if ((memory == nullptr) \|\| (source == nullptr) \|\| (written == nullptr)) {` | no |
| examples/ek_ra8d2/hw_pending/media_download/media_download_format.c | 2 | (file scope) | `if ((offset > memory->capacity) \|\| (requested > ((uint64_t)memory->capacity...` | no |
| examples/ek_ra8d2/hw_pending/media_download/media_download_format.c | 3 | (file scope) | `if ((context == nullptr) \|\| (destination == nullptr) \|\| (out_read == null...` | no |
| examples/ek_ra8d2/hw_pending/media_download/media_download_format.c | 2 | (file scope) | `if ((offset > memory->length) \|\| (requested > (memory->length - offset))) {` | no |
| examples/ek_ra8d2/hw_pending/media_download/media_download_format.c | 4 | (file scope) | `if ((source == nullptr) \|\| (config == nullptr) \|\| (workspace == nullptr) ...` | no |
| examples/ek_ra8d2/hw_pending/media_download/media_download_format.c | 3 | (file scope) | `if ((workspace->image == nullptr) \|\| (workspace->flat == nullptr) \|\|` | no |
| examples/ek_ra8d2/hw_pending/media_download/media_download_format.c | 4 | (file scope) | `if ((source_size == 0U) \|\| (workspace->image_cap == 0U) \|\| (workspace->fl...` | no |
| examples/ek_ra8d2/hw_pending/media_download/media_download_format.c | 2 | (file scope) | `if ((result == k_ra8_ok) && (flat_size != flat.length)) {` | no |
| examples/ek_ra8d2/hw_pending/media_download/media_download_format.c | 2 | (file scope) | `if ((result == k_ra8_ok) && (*packed_size != packed.length)) {` | no |
| examples/ek_ra8d2/hw_validated/c6/wifi_hal_join/src/wifi_hal_core.c | 2 | wifi_hal_settle | `if ((ra8_wifi_status(wifi, &st) == k_ra8_ok) && st.associated) {` | partial |
| examples/ek_ra8d2/hw_validated/c6/wifi_hal_join/src/wifi_hal_core.c | 2 | wifi_hal_settle | `return (ra8_wifi_status(wifi, &st) == k_ra8_ok) && st.associated;` | no |
| examples/ek_ra8d2/hw_validated/c6/wifi_hal_join/src/wifi_hal_core.c | 4 | wifi_hal_join_run | `if ((cfg == nullptr) \|\| (cfg->wifi == nullptr) \|\| (cfg->wifi_cfg == nullp...` | partial |
| examples/ek_ra8d2/hw_validated/c6/wifi_hal_join/src/wifi_hal_core.c | 2 | wifi_hal_join_run | `if ((out->ip_err != k_ra8_ok) \|\| !out->lease.bound) {` | partial |
| port/esp-hosted/src/ra8_esp_hosted_fmt.c | 2 | internal_put | `if ((cur == nullptr) \|\| (cur->out == nullptr)) {` | no |
| port/esp-hosted/src/ra8_esp_hosted_gpio.c | 2 | internal_isr_trampoline | `if ((row == nullptr) \|\| (row->handler == nullptr)) {` | no |
| port/esp-hosted/src/ra8_esp_hosted_rtos.c | 2 | internal_copy_name | `if ((dst == nullptr) \|\| (cap == 0U)) {` | no |
| port/esp-hosted/src/ra8_esp_hosted_rtos.c | 2 | internal_thread_entry | `if (s_rtos.thread_used[idx] && (s_rtos.threads[idx].entry != nullptr)) {` | no |
| port/esp-hosted/src/ra8_esp_hosted_rtos.c | 2 | internal_timer_expiry | `if (s_rtos.timer_used[idx] && (s_rtos.timers[idx].cb_fn != nullptr)) {` | no |
| port/esp-hosted/src/ra8_esp_hosted_rtos.c | 2 | internal_spin_us | `for (uint32_t i = 0U; (i < iters) && (i < (uint32_t)k_ra8_esp_hosted_spin_ite...` | partial |
| port/esp-hosted/src/ra8_esp_hosted_rtos.c | 2 | internal_h_blocking_delay | `for (uint32_t i = 0U; (i < iters) && (i < (uint32_t)k_ra8_esp_hosted_delay_it...` | partial |
| port/esp-hosted/src/ra8_esp_hosted_rtos.c | 3 | internal_h_blocking_delay | `if (!s_rtos.ready \|\| (timeout_handler == nullptr) \|\| (duration_ms <= 0)) {` | partial |
| port/esp-hosted/src/ra8_esp_hosted_rtos_sync.c | 2 | internal_h_create_semaphore | `if (!s_sync.ready \|\| (max_count <= 0)) {` | partial |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_ra8_esp_hosted_tx_shim_set_ticks | `if ((status == nullptr) \|\| (idx >= (uint32_t)k_ra8_esp_hosted_tx_shim_famil...` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_ra8_esp_hosted_tx_shim_set_ticks | `if ((status == nullptr) \|\| (idx >= (uint32_t)k_ra8_esp_hosted_tx_shim_famil...` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_ra8_esp_hosted_tx_shim_set_ticks | `if ((status == nullptr) \|\| (idx >= (uint32_t)k_ra8_esp_hosted_tx_shim_famil...` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_ra8_esp_hosted_tx_shim_set_ticks | `if ((status == nullptr) \|\| (idx >= (uint32_t)k_ra8_esp_hosted_tx_shim_famil...` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_ra8_esp_hosted_tx_shim_set_ticks | `if ((status == nullptr) \|\| (idx >= (uint32_t)k_ra8_esp_hosted_tx_shim_famil...` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_ra8_esp_hosted_tx_shim_set_ticks | `if ((status == nullptr) \|\| (idx >= (uint32_t)k_ra8_esp_hosted_tx_shim_famil...` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_ra8_esp_hosted_tx_shim_set_ticks | `if ((status == nullptr) \|\| (idx >= (uint32_t)k_ra8_esp_hosted_tx_shim_famil...` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_ra8_esp_hosted_tx_shim_set_ticks | `if ((status == nullptr) \|\| (idx >= (uint32_t)k_ra8_esp_hosted_tx_shim_famil...` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_ra8_esp_hosted_tx_shim_fire_timer | `if ((timer_ptr == nullptr) \|\| (timer_ptr->magic != (uint32_t)k_ra8_esp_host...` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_ra8_esp_hosted_tx_shim_fire_timer | `if ((timer_ptr == nullptr) \|\| (timer_ptr->magic != (uint32_t)k_ra8_esp_host...` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_ra8_esp_hosted_tx_shim_fire_timer | `if (!timer_ptr->active \|\| (timer_ptr->expiry == nullptr)) {` | partial |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_ra8_esp_hosted_tx_shim_fire_timer | `if (!timer_ptr->active \|\| (timer_ptr->expiry == nullptr)) {` | partial |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_ra8_esp_hosted_tx_shim_fire_timer | `if ((pool_ptr == nullptr) \|\| (pool_start == nullptr)) {` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_ra8_esp_hosted_tx_shim_fire_timer | `if ((pool_ptr == nullptr) \|\| (pool_start == nullptr)) {` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_tx_byte_pool_delete | `if ((pool_ptr == nullptr) \|\| (pool_ptr->magic != (uint32_t)k_ra8_esp_hosted...` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_tx_byte_pool_delete | `if ((pool_ptr == nullptr) \|\| (pool_ptr->magic != (uint32_t)k_ra8_esp_hosted...` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_tx_byte_pool_delete | `if ((pool_ptr == nullptr) \|\| (pool_ptr->magic != (uint32_t)k_ra8_esp_hosted...` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_tx_byte_pool_delete | `if ((pool_ptr == nullptr) \|\| (pool_ptr->magic != (uint32_t)k_ra8_esp_hosted...` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_tx_byte_pool_delete | `if ((need > pool_ptr->size) \|\| (cursor > (pool_ptr->size - need))) {` | partial |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_tx_byte_pool_delete | `if ((need > pool_ptr->size) \|\| (cursor > (pool_ptr->size - need))) {` | partial |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_tx_byte_release | `if ((pool_ptr == nullptr) \|\| (pool_ptr->magic != (uint32_t)k_ra8_esp_hosted...` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_tx_byte_release | `if ((pool_ptr == nullptr) \|\| (pool_ptr->magic != (uint32_t)k_ra8_esp_hosted...` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_tx_byte_release | `if ((queue_ptr == nullptr) \|\| (queue_start == nullptr)) {` | no |
| port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h | 2 | internal_tx_byte_release | `if ((queue_ptr == nullptr) \|\| (queue_start == nullptr)) {` | no |
| ... | | | | *(43 more rows in CSV)* | |

## Deactivated gaps (DO-178C 6.4.4.3 exempted)

These conditions are unreachable on any public-API path and are therefore exempted from the MC/DC gate. Each row carries the rationale used by the auto-classifier; humans may extend the per-condition narrative in `docs/MCDC_DEACTIVATIONS.md`.

| File | Conds | Function | Excerpt | Rationale |
|------|------:|----------|---------|-----------|
| libs/ra8_app/src/ra8_app.c | 2 | (file scope) | `if ((next != nullptr) && (next->vt->on_enter != nullptr)) {` | Annotated deactivation: next=reg-&gt;apps[target] with targe... |
| libs/ra8_book/src/ra8_book_chunked_validate.c | 2 | internal_chunk_flat_read | `if ((offset > ctx->rd->inflated_total) \|\| ((uint64_t)le...` | Annotated deactivation: internal_chunk_flat_read range gu... |
| libs/ra8_book/src/ra8_book_stream.c | 3 | internal_validate_raster | `if ((expect > (uint64_t)UINT32_MAX) \|\|` | Annotated deactivation: internal_validate_raster overflow... |
| libs/ra8_box/src/ra8_box.c | 2 | internal_iter_live | `return (link != (int32_t)k_ra8_box_none) && (guard < count);` | Annotated deactivation: guard&lt;count is an acyclic-tree cy... |
| libs/ra8_c6link/src/ra8_c6link_mdl.c | 2 | internal_mdl_take_response | `if ((body->data.data == nullptr) \|\| (body->data.len == ...` | Annotated deactivation: internal_mdl_take_response empty-... |
| libs/ra8_c6link/src/ra8_c6link_mdl.c | 3 | ra8_c6link_mdl_chunk_semantics_valid_test | `if ((packed == 0U) \|\| (packed > sizeof(link->mdl_reques...` | Annotated deactivation: ra8_c6link_mdl_start_request code... |
| libs/ra8_c6link/src/ra8_c6link_mdl.c | 3 | ra8_c6link_mdl_chunk_semantics_valid_test | `if ((packed == 0U) \|\| (packed > sizeof(link->mdl_reques...` | Annotated deactivation: ra8_c6link_mdl_next codec self-co... |
| libs/ra8_c6link/src/ra8_c6link_mdl.c | 3 | ra8_c6link_mdl_cancel | `if ((packed == 0U) \|\| (packed > sizeof(link->mdl_reques...` | Annotated deactivation: ra8_c6link_mdl_cancel codec self-... |
| libs/ra8_c6link/src/ra8_c6link_mdl_transfer.c | 2 | (file scope) | `} else if ((err == k_ra8_ok) && (chunk.state == k_ra8_mdl...` | Annotated deactivation: ra8_c6link_mdl_transfer CANCELLED... |
| libs/ra8_c6link/src/ra8_c6link_mdl_transfer.c | 2 | (file scope) | `if ((err == k_ra8_ok) && state.session.active) {` | Annotated deactivation: ra8_c6link_mdl_transfer exhausted... |
| libs/ra8_core/src/ra8_log.c | 2 | internal_itm_put_u32 | `while (value != 0U && i < k_ra8_u32_max_digits) {` | Annotated deactivation: internal_itm_put_u32 digit-buffer... |
| libs/ra8_dfu/src/ra8_rot.c | 2 | internal_ct_equal | `if ((psa_err != k_ra8_ok) && (psa_err != k_ra8_err_exists...` | Annotated deactivation: DO-178C 6.4.4.3 -- under RA8_OFF_... |
| libs/ra8_epub/src/ra8_epub_fs.c | 3 | internal_fs_stream_read | `if (io == nullptr \|\| io->file == nullptr \|\| buf == nu...` | TU-local static helper `internal_fs_stream_read` -- defen... |
| libs/ra8_epub/src/ra8_epub_open.c | 2 | internal_dirname | `if (dst == nullptr \|\| cap == 0U) {` | TU-local static helper `internal_dirname` -- defensive NU... |
| libs/ra8_epub/src/ra8_epub_open.c | 2 | internal_stream_read | `if (sm == nullptr \|\| sm->read == nullptr) {` | TU-local static helper `internal_stream_read` -- defensiv... |
| libs/ra8_epub/src/ra8_epub_open.c | 2 | internal_finish_open | `if (zip == nullptr \|\| out_book == nullptr) {` | TU-local static helper `internal_finish_open` -- defensiv... |
| libs/ra8_epub/src/ra8_epub_xml_shim.c | 2 | (file scope) | `if ((ra8_xml_attr_next(source, source_len, event, &cursor...` | Annotated deactivation: priv_ra8_epub_xml_attr attribute ... |
| libs/ra8_epub/src/ra8_epub_xml_shim.c | 3 | priv_ra8_epub_xml_ancestor_frame | `return ((err == k_ra8_ok) && ((manifest_depth == UINT16_M...` | Annotated deactivation: internal_opf_status package-shape... |
| libs/ra8_epub/src/ra8_epub_xml_shim.c | 2 | priv_ra8_epub_xml_ancestor_frame | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_ra8_...` | Annotated deactivation: internal_opf_first pull-loop stat... |
| libs/ra8_epub/src/ra8_epub_xml_shim.c | 2 | priv_ra8_epub_xml_ancestor_frame | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_ra8_...` | Annotated deactivation: internal_collect_spine pull-loop ... |
| libs/ra8_epub/src/ra8_epub_xml_shim.c | 2 | priv_ra8_epub_xml_ancestor_frame | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_ra8_...` | Annotated deactivation: internal_manifest_lookup pull-loo... |
| libs/ra8_epub/src/ra8_epub_xml_shim.c | 2 | priv_ra8_epub_xml_ancestor_frame | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_ra8_...` | Annotated deactivation: internal_opf_shape pull-loop stat... |
| libs/ra8_epub/src/ra8_epub_xml_shim.c | 3 | priv_ra8_epub_xml_ancestor_frame | `if ((err == k_ra8_ok) && (!saw_manifest \|\| !saw_spine)) {` | Annotated deactivation: internal_opf_shape completeness g... |
| libs/ra8_epub/src/ra8_epub_xml_shim.c | 2 | priv_ra8_epub_xml_ancestor_frame | `for (uint16_t i = 0U; (err == k_ra8_ok) && (i < book->xml...` | Annotated deactivation: internal_opf_resolve_refs spine-r... |
| libs/ra8_epub/src/ra8_epub_xml_shim.c | 3 | priv_ra8_epub_xml_ancestor_frame | `if ((err == k_ra8_ok) && (book->cover_path[0] == '\0') &&` | Annotated deactivation: internal_opf_resolve_refs legacy-... |
| libs/ra8_epub/src/ra8_epub_xml_shim.c | 3 | priv_ra8_epub_xml_ancestor_frame | `if ((err == k_ra8_ok) && (book->toc_kind != (uint8_t)k_ra...` | Annotated deactivation: internal_opf_resolve_refs spine `...` |
| libs/ra8_epub/src/ra8_epub_xml_toc.c | 2 | internal_toc_marker | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_ra8_...` | Annotated deactivation: internal_toc_capacity pull-loop s... |
| libs/ra8_epub/src/ra8_epub_xml_toc.c | 2 | internal_ncx_event | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_ra8_...` | Annotated deactivation: priv_ra8_epub_xml_parse_ncx pull-... |
| libs/ra8_epub/src/ra8_epub_xml_toc.c | 2 | internal_ncx_event | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_ra8_...` | Annotated deactivation: internal_select_nav pull-loop sta... |
| libs/ra8_epub/src/ra8_epub_xml_toc.c | 2 | internal_ncx_event | `if ((err == k_ra8_ok) && (fallback.kind != (uint8_t)k_ra8...` | Annotated deactivation: internal_select_nav fallback gate... |
| libs/ra8_epub/src/ra8_epub_xml_toc.c | 2 | internal_ncx_event | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_ra8_...` | Annotated deactivation: internal_nav_has_list pull-loop s... |
| libs/ra8_epub/src/ra8_epub_xml_toc.c | 2 | internal_nav_event | `} else if ((event->kind == (uint8_t)k_ra8_xml_event_text)...` | Annotated deactivation: internal_nav_event text-depth gat... |
| libs/ra8_epub/src/ra8_epub_xml_toc.c | 2 | internal_nav_event | `if ((err != k_ra8_ok) \|\| (event.kind == (uint8_t)k_ra8_...` | Annotated deactivation: priv_ra8_epub_xml_parse_nav pull-... |
| libs/ra8_epub/src/ra8_epub_xml_toc.c | 2 | internal_nav_event | `if ((err != k_ra8_ok) \|\| !ctx.saw_ol) {` | Annotated deactivation: priv_ra8_epub_xml_parse_nav compl... |
| libs/ra8_fs/src/ra8_fs_fat_name.c | 3 | priv_83_to_str | `if (j > 0 && (uint8_t)out13[0] == k_dir_marker_kanji_e5 &...` | Annotated deactivation: 3-condition AND on Shift-JIS kanj... |
| libs/ra8_hal/src/ra8_ble.c | 2 | internal_dispatch_event | `if ((internal_rx_byte(&code) == 0U) \|\| (internal_rx_byt...` | Annotated deactivation: TU-local helper internal_dispatch... |
| libs/ra8_hal/src/ra8_ble.c | 4 | internal_dispatch_acl | `if ((internal_rx_byte(&hdl_lo) == 0U) \|\| (internal_rx_b...` | Annotated deactivation: TU-local helper internal_dispatch... |
| libs/ra8_hal/src/ra8_canfd_timing.c | 2 | (file scope) | `if ((bitrate_bps == 0U) \|\| (clock_hz == 0U)) {` | Annotated deactivation: both args are validated by ra8_ca... |
| libs/ra8_hal/src/ra8_canfd_timing.c | 2 | (file scope) | `if ((prescaler < k_ra8_canfd_prescaler_min) \|\| (prescal...` | Annotated deactivation: ra8_canfd_deinit (bit-timing solv... |
| libs/ra8_hal/src/ra8_ceu.c | 2 | internal_arm_capture | `if ((s_ceu_image_area != 0U) && (bufs->y_top != nullptr)) {` | Annotated deactivation: TU-local helper internal_arm_capt... |
| libs/ra8_hal/src/ra8_dotf.c | 2 | internal_check_overlap | `if ((region->start_addr <= live->end_addr) && (live->star...` | Annotated deactivation: ra8_dotf overlap-detection AND; t... |
| libs/ra8_hal/src/ra8_eth.c | 2 | priv_ra8_eth_channel_to_port | `if ((tx == 0U) \|\| (tx > k_ra8_eth_num_tx_desc)) {` | Annotated deactivation: tx normalized to nonzero above; f... |
| libs/ra8_hal/src/ra8_eth.c | 2 | priv_ra8_eth_channel_to_port | `if ((rx == 0U) \|\| (rx > k_ra8_eth_num_rx_desc)) {` | Annotated deactivation: rx normalized to nonzero above; f... |
| libs/ra8_hal/src/ra8_flash_irq.c | 2 | ra8_flash_blank_check | `(address >= k_ra8_flash_code_start) &&` | Annotated deactivation: ra8_flash_blank_check window-memb... |
| libs/ra8_hal/src/ra8_i3c_i2c.c | 2 | internal_i3c_i2c_half_period | `if ((bus_hz == 0U) \|\| (pclka_hz == 0U)) {` | Annotated deactivation: both args validated by ra8_i3c_i2... |
| libs/ra8_hal/src/ra8_mipi_dsi_dispatch.c | 2 | ra8_mipi_dsi_dispatch_receive | `if ((s_mipi_dsi_pending_rx_buffer != nullptr) && (s_mipi_...` | Annotated deactivation: ra8_mipi_dsi_dispatch_receive pen... |
| libs/ra8_hal/src/ra8_mipi_phy_timing.c | 3 | (file scope) | `if ((tbl[i].mode == mode_flag) && (tbl[i].pclka == pclka)...` | Annotated deactivation: TU-local helper internal_mipi_phy... |
| libs/ra8_hal/src/ra8_rmac_mgmt.c | 2 | ra8_rmac_phy_auto_neg_start | `if (out_link->up && ((bmsr & (uint16_t)k_ra8_rmac_phy_bms...` | Annotated deactivation: ra8_rmac_phy_auto_neg_start link-... |
| libs/ra8_hal/src/ra8_spi_b.c | 2 | internal_unit_bytes | `if ((tx == nullptr) && (rx == nullptr)) {` | Annotated deactivation: TU-local helper internal_apply_bi... |
| libs/ra8_hal/src/ra8_usb_cdc.c | 2 | internal_apply_line_coding | `if ((data == nullptr) \|\| (len < k_ra8_cdc_line_coding_l...` | Annotated deactivation: TU-local helper internal_apply_li... |
| libs/ra8_hal/src/ra8_vin.c | 2 | internal_mc_rmw | `if (((mc_now & k_ra8_vin_mc_me) != 0UL) \|\| ((fc_now & k...` | Annotated deactivation: ra8_vin idle-state guard; MC.ME (... |
| libs/ra8_jof/src/ra8_jof_png.c | 2 | internal_png_inflate_step | `if ((it->in_pos == it->in_avail) && (st->source_done == 0...` | Annotated deactivation: refill-first loop structure; ente... |
| libs/ra8_jof/src/ra8_jof_png.c | 2 | internal_png_inflate_step | `if ((in_sz == 0U) && (out_sz == 0U)) {` | Annotated deactivation: zero-progress stall guard, defens... |
| libs/ra8_jof/src/ra8_jof_png.c | 2 | internal_png_inflate_step | `if ((it->stalls > 1U) \|\| (st->source_done != 0U)) {` | Annotated deactivation: inner arm of the non-constructibl... |
| libs/ra8_jof/src/ra8_jof_produce.c | 2 | internal_carve_pixel_path | `if ((band_bytes > (uint64_t)UINT32_MAX) \|\| (stage_bytes...` | Annotated deactivation: stage_bytes = tw_eff*th_eff*bpp w... |
| libs/ra8_jof/src/ra8_jof_produce.c | 3 | internal_flush_band | `if ((st->geom_done == 0U) \|\| (width != st->w) \|\| (cha...` | Annotated deactivation: row-sink contract guard; both in-... |
| libs/ra8_jof/src/ra8_jof_produce.c | 3 | internal_flush_band | `if ((nrows == 0U) \|\| ((uint32_t)y0 != st->rows_seen) \|\|` | Annotated deactivation: row-ordering contract guard; the ... |
| libs/ra8_jof/src/ra8_jof_produce.c | 2 | internal_epilogue | `if ((st->geom_done == 0U) \|\| (st->rows_seen != (uint32_...` | Annotated deactivation: post-decode contract guard; both ... |
| libs/ra8_jpeg/src/ra8_jpeg_sw.c | 4 | priv_jpeg_sw_idct8x8 | `if (mk >= k_jpeg_marker_sof_lo && mk <= k_jpeg_marker_sof...` | Annotated deactivation: internal_dims_step unsupported-SO... |
| libs/ra8_jpeg/src/ra8_jpeg_sw_decode.c | 2 | priv_jpeg_sw_parse_dqt | `if (len < 2U \|\| (uint32_t)len > d->src_len - d->cursor) {` | Defensive segment-length bound in a bounded parser: buffe... |
| libs/ra8_jpeg/src/ra8_jpeg_sw_decode.c | 2 | priv_jpeg_sw_parse_sof0 | `if (len < 8U \|\| (uint32_t)len > d->src_len - d->cursor) {` | Defensive segment-length bound in a bounded parser: buffe... |
| libs/ra8_jpeg/src/ra8_jpeg_sw_decode.c | 2 | priv_jpeg_sw_parse_sos | `if (len < 6U \|\| (uint32_t)len > d->src_len - d->cursor) {` | Defensive segment-length bound in a bounded parser: buffe... |
| libs/ra8_jpeg/src/ra8_jpeg_sw_decode.c | 2 | priv_jpeg_sw_parse_sos | `if (r < 0 && t != 0) {` | Annotated deactivation: priv_jpeg_sw_block priv_jpeg_sw_b... |
| libs/ra8_longstrip/src/ra8_longstrip.c | 2 | ra8_longstrip_tick | `if ((wt->velocity == 0) \|\| internal_fling_should_stop(w...` | Pointer(s) ['wt'] already null-checked upstream in the sa... |
| libs/ra8_net_pal/src/ra8_net_pal.c | 2 | internal_eth_event | `if ((s_state.event_fn != nullptr) && (pal_mask != k_ra8_n...` | Annotated deactivation: TU-local helper internal_eth_even... |
| libs/ra8_psa_crypto/src/ra8_psa_crypto_fake.c | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < key_len) && (off < sizeof(buf));...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra8_psa_crypto/src/ra8_psa_crypto_fake.c | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < nonce_len) && (off < sizeof(buf)...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra8_psa_crypto/src/ra8_psa_crypto_fake.c | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < aad_len) && (off < sizeof(buf));...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra8_psa_crypto/src/ra8_psa_crypto_fake.c | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < key_len) && (off < sizeof(seed))...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra8_psa_crypto/src/ra8_psa_crypto_fake.c | 2 | internal_sha256_rotr | `for (size_t i = 0U; (i < nonce_len) && (off < sizeof(seed...` | Defensive scratch-buffer bound: input length is capped by... |
| libs/ra8_reflow/src/ra8_reflow_css.c | 3 | internal_ci_contains | `if ((s == nullptr) \|\| (sl == 0U) \|\| (sl > len)) {` | Annotated deactivation: the sole caller passes internal_c... |
| libs/ra8_reflow/src/ra8_reflow_css.c | 2 | internal_parse_color | `if ((s == nullptr) \|\| (len == 0U)) {` | Annotated deactivation: the sole caller internal_apply_de... |
| libs/ra8_reflow/src/ra8_reflow_css_cascade.c | 4 | internal_rule_rank | `if ((!have) \|\| (rank > best_rank) \|\|` | Annotated deactivation: rules[].order is assigned monoton... |
| libs/ra8_reflow/src/ra8_reflow_css_cascade.c | 3 | internal_ci_eq_span | `if ((a == nullptr) \|\| (b == nullptr) \|\| (alen != blen...` | Annotated deactivation: internal_ci_eq_span is reached on... |
| libs/ra8_reflow/src/ra8_reflow_css_rules.c | 2 | internal_intern_name | `if ((len == 0U) \|\| (len > (size_t)k_ra8_css_name_max)) {` | Annotated deactivation: every caller guards the length be... |
| libs/ra8_reflow/src/ra8_reflow_css_rules.c | 2 | internal_parse_sel_type | `while ((i < len) && priv_ra8_reflow_css_is_ws(s[i])) {` | Annotated deactivation: internal_split_compounds is only ... |
| libs/ra8_reflow/src/ra8_reflow_image.c | 2 | internal_decode_fail | `if ((reason != nullptr) && (strstr(reason, "outofmem") !=...` | Annotated deactivation: stbi sets a reason on every failu... |
| libs/ra8_reflow/src/ra8_reflow_image.c | 3 | internal_arena_release | `if ((pixels == nullptr) \|\| (sx <= 0) \|\| (sy <= 0)) {` | Annotated deactivation: stbi guarantees sx,sy &gt;= 1 when p... |
| libs/ra8_reflow/src/ra8_reflow_layout_image.c | 3 | internal_page_has_content | `if ((ra8_img_probe_size(bytes, blen, &iw, &ih) != k_ra8_o...` | Annotated deactivation: ra8_img_probe_size returns k_ra8_... |
| libs/ra8_reflow/src/ra8_reflow_layout_image.c | 2 | internal_page_has_content | `if (((cur->y + (int32_t)cur->line_height_px) > bottom_lim...` | Annotated deactivation: internal_image_record incremented... |
| libs/ra8_reflow/src/ra8_reflow_layout_table.c | 2 | internal_table_columns | `if ((*cx > cell_x) && ((*cx + adv) <= cell_right)) {` | Annotated deactivation: priv_ra8_reflow_tok_stash_run col... |
| libs/ra8_reflow/src/ra8_reflow_render.c | 2 | internal_init_font | `if ((w > 0) && (h > 0)) {` | Annotated deactivation: glyph bbox w,h co-dependent (inke... |
| libs/ra8_reflow/src/ra8_reflow_svg_doc.c | 2 | internal_xform_with_group | `const bool self_close = (close > i) && (s[close - 1U] == ...` | Annotated deactivation: close&gt;i is invariant for a named ... |
| libs/ra8_reflow/src/ra8_reflow_svg_path.c | 2 | internal_next_cmd | `const bool    rel = (c >= 'a') && (c <= 'z');` | Annotated deactivation: c is a command letter or 0; (c&lt;='... |
| libs/ra8_reflow/src/ra8_reflow_svg_shape.c | 2 | internal_sort_i32 | `for (int32_t i = 0; (i < n) && (m < (int32_t)k_svg_poly_m...` | Annotated deactivation: the polygon point count n is itse... |
| libs/ra8_reflow/src/ra8_reflow_svg_shape.c | 4 | internal_sort_i32 | `if (((y0 <= y) && (y < y1)) \|\| ((y1 <= y) && (y < y0))) {` | Annotated deactivation: the fourth condition (y &lt; y0) is ... |
| libs/ra8_reflow/src/ra8_reflow_svg_shape.c | 2 | internal_grad_eval | `if ((p >= o0) && (p <= o1)) {` | Annotated deactivation: the loop is entered only when p &gt;... |
| libs/ra8_reflow/src/ra8_reflow_tokenize_lex.c | 3 | priv_ra8_reflow_tok_classify | `if ((i < avail) && ((src[i] == 'x') \|\| (src[i] == 'X'))) {` | Annotated deactivation: the sole caller priv_ra8_reflow_t... |
| libs/ra8_reflow/src/ra8_reflow_tokenize_lex.c | 3 | priv_ra8_reflow_tok_classify | `if ((digits == 0U) \|\| (i >= avail) \|\| (src[i] != ';')) {` | Annotated deactivation: the scan loop above exits with i ... |
| libs/ra8_touch_cal/src/ra8_touch_cal.c | 2 | internal_clip32 | `if (!ok_u \|\| !ok_v) {` | Annotated deactivation: TU-local helper internal_clip32 s... |
| libs/ra8_wdt_supervisor/src/ra8_wdt_supervisor.c | 2 | ra8_wdt_supervisor_tick | `if (will_refresh && (s_state.refresh != nullptr)) {` | Annotated deactivation: ra8_wdt_supervisor_tick refresh d... |
| libs/ra8_xml/src/ra8_xml.c | 3 | ra8_xml_attr_begin | `if ((err != k_ra8_ok) \|\| (count == UINT16_MAX) \|\|` | Annotated deactivation: internal_attributes attribute-cou... |
| libs/ra8_xml/src/ra8_xml.c | 2 | ra8_xml_attr_begin | `if ((cursor.position >= end) \|\| (source[cursor.position...` | Annotated deactivation: internal_attributes self-closing ... |
| libs/ra8_xml/src/ra8_xml.c | 5 | internal_encoding | `if (!initial \|\| (reader->declaration_seen != 0U) \|\| (...` | Annotated deactivation: internal_declaration placement ga... |
| libs/ra8_xml/src/ra8_xml.c | 2 | internal_cdata | `if (((pos + 2U) <= reader->source_len) && (reader->source...` | Annotated deactivation: internal_special processing-instr... |
| libs/ra8_xml/src/ra8_xml.c | 3 | ra8_xml_reader_next | `if ((reader->stack_size != 0U) \|\| (reader->root_count !...` | Annotated deactivation: ra8_xml_reader_next end-of-docume... |
| libs/ra8_xml/src/ra8_xml_decode.c | 3 | internal_digit | `if ((cursor < end) && ((source[cursor] == (uint8_t)'x') \...` | Annotated deactivation: internal_entity radix probe; the ... |
| libs/ra8_xml/src/ra8_xml_decode.c | 3 | internal_digit | `if ((internal_decoded_byte(source, &li, &lb) != k_ra8_ok)...` | Annotated deactivation: ra8_xml_decoded_equal byte-walk s... |
| libs/ra8_xml/src/ra8_xml_doctype.c | 2 | internal_pubid_byte | `if (((*position + length) > end) \|\| (memcmp(&source[*po...` | Annotated deactivation: internal_keyword prefix recheck; ... |

## Per-module gap counts (full table)

Sorted by (uncovered + partial) descending, then total descending.

| Module | Total | Covered | Partial | Uncovered |
|--------|------:|--------:|--------:|----------:|
| ra8_esp_hosted_tx_shim_internal | 36 | 0 | 4 | 32 |
| ra8_esp_hosted_tx_shim_sync_internal | 28 | 0 | 0 | 28 |
| ra8_epub_xml_shim | 38 | 28 | 8 | 2 |
| mg_reader | 11 | 1 | 4 | 6 |
| media_download_format | 9 | 0 | 0 | 9 |
| ra8_epub_xml_toc | 32 | 24 | 5 | 3 |
| ra8_esp_hosted_rtos | 15 | 9 | 3 | 3 |
| ra8_xml | 55 | 50 | 4 | 1 |
| ra8_psa_crypto_fake | 6 | 1 | 5 | 0 |
| ra8_c6link_mdl | 24 | 20 | 1 | 3 |
| ra8_jof_produce | 19 | 15 | 1 | 3 |
| ra8_jpeg_sw_decode | 16 | 12 | 4 | 0 |
| ez_scene | 7 | 3 | 4 | 0 |
| wifi_hal_core | 4 | 0 | 3 | 1 |
| ra8_reflow_svg_shape | 15 | 12 | 3 | 0 |
| mdl_service | 12 | 9 | 2 | 1 |
| ra8_epub_open | 9 | 6 | 0 | 3 |
| ra8_jof_png | 7 | 4 | 1 | 2 |
| ra8_reflow_css | 26 | 24 | 1 | 1 |
| ra8_xml_decode | 25 | 23 | 2 | 0 |
| ra8_reflow_css_cascade | 23 | 21 | 2 | 0 |
| ra8_reflow_css_rules | 21 | 19 | 2 | 0 |
| ra8_reflow_tokenize_lex | 11 | 9 | 2 | 0 |
| ra8_c6link_mdl_transfer | 10 | 8 | 2 | 0 |
| ra8_ble | 8 | 6 | 0 | 2 |
| ra8_reflow_layout_image | 6 | 4 | 2 | 0 |
| ra8_eth | 4 | 2 | 2 | 0 |
| ra8_canfd_timing | 3 | 1 | 2 | 0 |
| ra8_reflow_image | 3 | 1 | 2 | 0 |
| ra8_book_stream | 22 | 21 | 1 | 0 |
| ra8_reflow_svg_doc | 20 | 19 | 1 | 0 |
| ra8_fs_fat_name | 14 | 13 | 1 | 0 |
| ra8_vin | 14 | 13 | 1 | 0 |
| ra8_touch_cal | 13 | 12 | 1 | 0 |
| ra8_xml_doctype | 13 | 12 | 0 | 1 |
| ra8_flash_irq | 9 | 8 | 1 | 0 |
| ra8_reflow_svg_path | 9 | 8 | 1 | 0 |
| ra8_esp_hosted_fmt | 8 | 7 | 0 | 1 |
| ra8_i3c_i2c | 7 | 6 | 0 | 1 |
| ra8_spi_b | 7 | 6 | 0 | 1 |
| ra8_app | 6 | 5 | 1 | 0 |
| ra8_dotf | 6 | 5 | 0 | 1 |
| ra8_jpeg_sw | 6 | 5 | 1 | 0 |
| ra8_mipi_dsi_dispatch | 6 | 5 | 1 | 0 |
| ra8_reflow_layout_table | 6 | 5 | 1 | 0 |
| ra8_rot | 6 | 5 | 0 | 1 |
| ra8_longstrip | 5 | 4 | 1 | 0 |
| ra8_book_chunked_validate | 4 | 3 | 0 | 1 |
| ra8_box | 4 | 3 | 1 | 0 |
| ra8_reflow_render | 4 | 3 | 1 | 0 |
| ra8_usb_cdc | 4 | 3 | 0 | 1 |
| ra8_ceu | 3 | 2 | 1 | 0 |
| ra8_epub_fs | 3 | 2 | 0 | 1 |
| ra8_esp_hosted_gpio | 3 | 2 | 0 | 1 |
| ra8_esp_hosted_rtos_sync | 3 | 2 | 1 | 0 |
| ra8_mipi_phy_timing | 3 | 2 | 1 | 0 |
| ra8_net_pal | 2 | 1 | 0 | 1 |
| ra8_rmac_mgmt | 2 | 1 | 0 | 1 |
| ra8_wdt_supervisor | 2 | 1 | 1 | 0 |
| ra8_log | 1 | 0 | 1 | 0 |
| fw_if_fs | 31 | 31 | 0 | 0 |
| ra8_reflow_tokenize | 24 | 24 | 0 | 0 |
| ra8_epub_chapter | 20 | 20 | 0 | 0 |
| ra8_reflow_svg | 20 | 20 | 0 | 0 |
| ra8_mipi_phy | 19 | 19 | 0 | 0 |
| ra8_psa_crypto | 15 | 15 | 0 | 0 |
| ra8_c6link | 14 | 14 | 0 | 0 |
| ra8_etha | 13 | 13 | 0 | 0 |
| ra8_book_xhtml | 12 | 12 | 0 | 0 |
| ra8_esp_hosted_rtos_pool | 12 | 12 | 0 | 0 |
| ra8_gfx_text | 12 | 12 | 0 | 0 |
| ra8_modem_at | 12 | 12 | 0 | 0 |
| ra8_reflow_layout_driver | 12 | 12 | 0 | 0 |
| ra8_reflow_svg_xform | 11 | 11 | 0 | 0 |
| ra8_reflow_tokenize_attr | 11 | 11 | 0 | 0 |
| ra8_book_paged | 10 | 10 | 0 | 0 |
| ra8_mipi_dsi_cmd | 10 | 10 | 0 | 0 |
| ra8_zoom | 10 | 10 | 0 | 0 |
| ra8_esp_hosted_osi_absent | 9 | 9 | 0 | 0 |
| ra8_fs_fat_mount | 9 | 9 | 0 | 0 |
| ra8_c6link_mdl_service | 8 | 8 | 0 | 0 |
| ra8_epaper_geom | 8 | 8 | 0 | 0 |
| ra8_pdg | 8 | 8 | 0 | 0 |
| ra8_rabook_comic | 8 | 8 | 0 | 0 |
| ra8_reflow_layout | 8 | 8 | 0 | 0 |
| ra8_rsip_asym | 8 | 8 | 0 | 0 |
| ra8_usb_xfer | 8 | 8 | 0 | 0 |
| ra8_widget | 8 | 8 | 0 | 0 |
| ra8_c6link_wifi_sta | 7 | 7 | 0 | 0 |
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
| fw_if_fs_dir | 6 | 6 | 0 | 0 |
| ra8_dfu_boot | 6 | 6 | 0 | 0 |
| ra8_epub_miniz_alloc | 6 | 6 | 0 | 0 |
| ra8_fs_fat_fmt | 6 | 6 | 0 | 0 |
| ra8_fs_fat_lfn | 6 | 6 | 0 | 0 |
| ra8_i2c | 6 | 6 | 0 | 0 |
| ra8_mipi_dsi | 6 | 6 | 0 | 0 |
| ra8_sci | 6 | 6 | 0 | 0 |
| ra8_ssie | 6 | 6 | 0 | 0 |
| ra8_usb_pal | 6 | 6 | 0 | 0 |
| ra8_usb_paud | 6 | 6 | 0 | 0 |
| ra8_usb_phid | 6 | 6 | 0 | 0 |
| ra8_c6link_tlv | 5 | 5 | 0 | 0 |
| ra8_drw_draw | 5 | 5 | 0 | 0 |
| ra8_fs_fat_label | 5 | 5 | 0 | 0 |
| ra8_fs_utf | 5 | 5 | 0 | 0 |
| ra8_gpt | 5 | 5 | 0 | 0 |
| ra8_i2c_peripheral | 5 | 5 | 0 | 0 |
| ra8_io_vfs_namespace | 5 | 5 | 0 | 0 |
| ra8_jof_png_chunk | 5 | 5 | 0 | 0 |
| ra8_rar5 | 5 | 5 | 0 | 0 |
| ra8_rar5_tables | 5 | 5 | 0 | 0 |
| ra8_usb_haud | 5 | 5 | 0 | 0 |
| ra8_usb_hcdc_ecm | 5 | 5 | 0 | 0 |
| ra8_usb_pprn | 5 | 5 | 0 | 0 |
| ra8_zoom_tiles | 5 | 5 | 0 | 0 |
| sec_cmac | 5 | 5 | 0 | 0 |
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
| ra8_reflow_cache | 4 | 4 | 0 | 0 |
| ra8_rsip | 4 | 4 | 0 | 0 |
| ra8_rsip_cipher | 4 | 4 | 0 | 0 |
| ra8_ssie_stream | 4 | 4 | 0 | 0 |
| ra8_unarch_tar_fields | 4 | 4 | 0 | 0 |
| ra8_usb_device | 4 | 4 | 0 | 0 |
| ra8_usb_hhid | 4 | 4 | 0 | 0 |
| ra8_usb_pvnd | 4 | 4 | 0 | 0 |
| adc_selfdiag | 3 | 3 | 0 | 0 |
| ra8_book | 3 | 3 | 0 | 0 |
| ra8_c6link_arena | 3 | 3 | 0 | 0 |
| ra8_dmac | 3 | 3 | 0 | 0 |
| ra8_esp_hosted_log | 3 | 3 | 0 | 0 |
| ra8_esp_hosted_port | 3 | 3 | 0 | 0 |
| ra8_esp_hosted_spi | 3 | 3 | 0 | 0 |
| ra8_eth_gwca_queue | 3 | 3 | 0 | 0 |
| ra8_fs_fat_dir | 3 | 3 | 0 | 0 |
| ra8_fs_fat_exfat_stream | 3 | 3 | 0 | 0 |
| ra8_fs_fat_file | 3 | 3 | 0 | 0 |
| ra8_jof | 3 | 3 | 0 | 0 |
| ra8_jpeg_sw_stream | 3 | 3 | 0 | 0 |
| ra8_ov5640 | 3 | 3 | 0 | 0 |
| ra8_rabook_import | 3 | 3 | 0 | 0 |
| ra8_reflow_link | 3 | 3 | 0 | 0 |
| ra8_sdmmc_spi | 3 | 3 | 0 | 0 |
| ra8_touch | 3 | 3 | 0 | 0 |
| ra8_ui | 3 | 3 | 0 | 0 |
| ra8_usb_hmsc | 3 | 3 | 0 | 0 |
| ra8_widget_keyboard | 3 | 3 | 0 | 0 |
| ra8_widget_panel | 3 | 3 | 0 | 0 |
| sh_classify | 3 | 3 | 0 | 0 |
| usb_printer_vendor_ch9 | 3 | 3 | 0 | 0 |
| adc | 2 | 2 | 0 | 0 |
| mkbookimg_names | 2 | 2 | 0 | 0 |
| ra8_agt | 2 | 2 | 0 | 0 |
| ra8_bkup_tamper | 2 | 2 | 0 | 0 |
| ra8_board_ek_ra8d2_audio_usb | 2 | 2 | 0 | 0 |
| ra8_comic_wrapped | 2 | 2 | 0 | 0 |
| ra8_devcfg | 2 | 2 | 0 | 0 |
| ra8_display_pal_policy | 2 | 2 | 0 | 0 |
| ra8_epaper_devinfo | 2 | 2 | 0 | 0 |
| ra8_epub_img_import | 2 | 2 | 0 | 0 |
| ra8_esp_hosted_gpio_edge | 2 | 2 | 0 | 0 |
| ra8_esp_hosted_osi | 2 | 2 | 0 | 0 |
| ra8_fs_fat_attr | 2 | 2 | 0 | 0 |
| ra8_gfx_dither | 2 | 2 | 0 | 0 |
| ra8_i3c_i2c_peripheral | 2 | 2 | 0 | 0 |
| ra8_io_stream | 2 | 2 | 0 | 0 |
| ra8_ipc | 2 | 2 | 0 | 0 |
| ra8_jof_produce_webp | 2 | 2 | 0 | 0 |
| ra8_jpeg_sw_encode | 2 | 2 | 0 | 0 |
| ra8_keyboard | 2 | 2 | 0 | 0 |
| ra8_rabook_pipeline | 2 | 2 | 0 | 0 |
| ra8_rmac_phy | 2 | 2 | 0 | 0 |
| ra8_rsip_ecc | 2 | 2 | 0 | 0 |
| ra8_rsip_rsa | 2 | 2 | 0 | 0 |
| ra8_sci_dma_isr | 2 | 2 | 0 | 0 |
| ra8_sdhi | 2 | 2 | 0 | 0 |
| ra8_spi_b_dma | 2 | 2 | 0 | 0 |
| ra8_tile_cache | 2 | 2 | 0 | 0 |
| ra8_tsn | 2 | 2 | 0 | 0 |
| ra8_usb_composite | 2 | 2 | 0 | 0 |
| ra8_usb_hcdc | 2 | 2 | 0 | 0 |
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
| board_periph_mstp_model | 1 | 1 | 0 | 0 |
| key_import | 1 | 1 | 0 | 0 |
| key_vault | 1 | 1 | 0 | 0 |
| ota_commit | 1 | 1 | 0 | 0 |
| ra8_board_ek_ra8d2_camera | 1 | 1 | 0 | 0 |
| ra8_c6link_pump | 1 | 1 | 0 | 0 |
| ra8_cache | 1 | 1 | 0 | 0 |
| ra8_canfd_frame | 1 | 1 | 0 | 0 |
| ra8_cgc_eswclk | 1 | 1 | 0 | 0 |
| ra8_comic_cbr | 1 | 1 | 0 | 0 |
| ra8_comic_tiles | 1 | 1 | 0 | 0 |
| ra8_crashlog | 1 | 1 | 0 | 0 |
| ra8_crc | 1 | 1 | 0 | 0 |
| ra8_display_pal_eink | 1 | 1 | 0 | 0 |
| ra8_display_pal_lcd | 1 | 1 | 0 | 0 |
| ra8_dual_core | 1 | 1 | 0 | 0 |
| ra8_epub_img_tiles | 1 | 1 | 0 | 0 |
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
| ra8_rabook_import_compiler | 1 | 1 | 0 | 0 |
| ra8_rabook_xml_shim | 1 | 1 | 0 | 0 |
| ra8_rar | 1 | 1 | 0 | 0 |
| ra8_reflow_parse | 1 | 1 | 0 | 0 |
| ra8_rtc | 1 | 1 | 0 | 0 |
| ra8_sci_lin | 1 | 1 | 0 | 0 |
| ra8_smbus | 1 | 1 | 0 | 0 |
| ra8_sram | 1 | 1 | 0 | 0 |
| ra8_systick | 1 | 1 | 0 | 0 |
| ra8_tz_secure_boot | 1 | 1 | 0 | 0 |
| ra8_unarch_tar | 1 | 1 | 0 | 0 |
| ra8_usb_pmsc | 1 | 1 | 0 | 0 |
| ra8_zoom_book | 1 | 1 | 0 | 0 |
| ra8_zoom_render | 1 | 1 | 0 | 0 |
| tx_systick_retune | 1 | 1 | 0 | 0 |

## Top 30 modules with at least one uncovered decision

| Module | Uncovered | Partial | Covered | Total |
|--------|----------:|--------:|--------:|------:|
| ra8_esp_hosted_tx_shim_internal | 32 | 4 | 0 | 36 |
| ra8_esp_hosted_tx_shim_sync_internal | 28 | 0 | 0 | 28 |
| media_download_format | 9 | 0 | 0 | 9 |
| mg_reader | 6 | 4 | 1 | 11 |
| ra8_epub_xml_toc | 3 | 5 | 24 | 32 |
| ra8_esp_hosted_rtos | 3 | 3 | 9 | 15 |
| ra8_c6link_mdl | 3 | 1 | 20 | 24 |
| ra8_jof_produce | 3 | 1 | 15 | 19 |
| ra8_epub_open | 3 | 0 | 6 | 9 |
| ra8_epub_xml_shim | 2 | 8 | 28 | 38 |
| ra8_jof_png | 2 | 1 | 4 | 7 |
| ra8_ble | 2 | 0 | 6 | 8 |
| ra8_xml | 1 | 4 | 50 | 55 |
| wifi_hal_core | 1 | 3 | 0 | 4 |
| mdl_service | 1 | 2 | 9 | 12 |
| ra8_reflow_css | 1 | 1 | 24 | 26 |
| ra8_book_chunked_validate | 1 | 0 | 3 | 4 |
| ra8_dotf | 1 | 0 | 5 | 6 |
| ra8_epub_fs | 1 | 0 | 2 | 3 |
| ra8_esp_hosted_fmt | 1 | 0 | 7 | 8 |
| ra8_esp_hosted_gpio | 1 | 0 | 2 | 3 |
| ra8_i3c_i2c | 1 | 0 | 6 | 7 |
| ra8_net_pal | 1 | 0 | 1 | 2 |
| ra8_rmac_mgmt | 1 | 0 | 1 | 2 |
| ra8_rot | 1 | 0 | 5 | 6 |
| ra8_spi_b | 1 | 0 | 6 | 7 |
| ra8_usb_cdc | 1 | 0 | 3 | 4 |
| ra8_xml_doctype | 1 | 0 | 12 | 13 |

---

*Regenerated from the live `make mcdc` report. See `docs/MCDC_GAPS.csv` for the full per-decision table including decision-text snippets and excerpts.*
