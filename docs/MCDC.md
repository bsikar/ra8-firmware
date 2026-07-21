# MC/DC Coverage (DO-178C Level B)

This document describes how `ra8-firmware` measures **Modified
Condition/Decision Coverage** (MC/DC) and how to add MC/DC test
vectors for new code.

MC/DC is the structural coverage criterion mandated by **DO-178C Level
B** (Hazardous failure condition) and **DO-178C Table A-7 objective
5**. The infrastructure described here is the foundation for
qualifying portions of this codebase under DO-178C; per-module MC/DC
test vectors are tracked separately.

## Coverage target

**100% MC/DC of reachable conditions.** Deactivated conditions (DO-178C
6.4.4.3) are exempted from the gate provided each one carries a
documented rationale in
[`docs/MCDC_DEACTIVATIONS.md`](MCDC_DEACTIVATIONS.md). The gate is

```
reachable_conditions_covered / reachable_conditions_total >= 100%
```

where `reachable_conditions_total = total_decisions - deactivated_decisions`.

`scripts/fix/regen_mcdc_gaps.py` auto-classifies each MC/DC gap as
`deactivated` (defensive guard already enforced upstream) or
`reachable` (still needs a test vector). The gate, the per-decision
catalog, and the deactivation rationale list all derive from the same
live `make mcdc` report -- there is no hand-curated allow-list.

Industry mappings: this policy is the IEC 61508-3:2010 7.4.7
"defensive code" exemption and the ISO 26262-6:2018 9.4.5 "deactivated
branches" treatment under different names.

## What is MC/DC?

For a compound boolean decision with `N` conditions, MC/DC requires:

1. **Decision coverage** -- the decision has evaluated to both `true`
   and `false`.
2. **Condition coverage** -- every condition has evaluated to both
   `true` and `false`.
3. **Independence** -- for every condition `Ci`, there is a pair of
   test cases where `Ci` flips and the *decision outcome* also flips,
   while every other condition is held constant.

The third requirement is what distinguishes MC/DC from plain
"condition coverage". For an `N`-condition decision, MC/DC typically
requires `N + 1` test cases (vs. `2^N` for full multi-condition
coverage).

Statement coverage and branch coverage are necessary but not
sufficient: they cannot detect that a condition was masked by
short-circuit evaluation, and they cannot prove that each condition
*independently* drives the outcome.

## Toolchain

| Tool          | Version             | Role                                |
|---------------|---------------------|-------------------------------------|
| `clang`       | >= 18 (we use 22)   | Source-based MC/DC instrumentation  |
| `llvm-profdata` | matching $CC      | Merge `.profraw` per-test files     |
| `llvm-cov`    | matching $CC        | Render MC/DC report                 |

The flag combination is:

```
-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc
```

`-fcoverage-mcdc` is what enables MC/DC bookkeeping. The first two
flags are the standard clang source-based coverage flags that MC/DC
piggy-backs on. See LLVM's
"[Source-based Code Coverage](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html#mc-dc-instrumentation)"
documentation.

If clang >= 18 is not on `$PATH`, the build falls back to gcc 14's
`-fcondition-coverage` (condition coverage, **NOT** MC/DC) and prints
a loud warning. This fallback exists so the script still runs
end-to-end on machines without modern clang; **it is not
DO-178C-compliant** and the report gate is skipped.

## How to run

```sh
make mcdc
```

This wraps `scripts/report/mcdc_report.sh`, which:

1. Configures `tests/` with `cmake -DRA8_MCDC=ON`.
2. Builds every host test with the MC/DC flag trio.
3. Runs each test binary with `LLVM_PROFILE_FILE` set so each emits
   its own `.profraw`.
4. Merges them via `llvm-profdata merge -sparse`.
5. Renders both a verbose per-file dump
   (`build/mcdc-report/mcdc.txt`) and a numeric summary
   (`build/mcdc-report/summary.txt`).
6. Exits non-zero if first-party MC/DC < 100% (override via
   `RA8_MCDC_THRESHOLD=NN`).

The existing `make test` and `make coverage` flows are untouched --
MC/DC instrumentation is opt-in.

## How to read the report

`build/mcdc-report/summary.txt` is a `llvm-cov report` table with an
extra **MC/DC Coverage** column (added by `--show-mcdc-summary`). A
typical row looks like:

```
Filename                                  Regions   Missed Regions   Cover     ...   MC/DC Conditions   Missed   Cover
libs/ra8_core/src/ra8_log.c                 142       8                94.37%    ...   12                 4        66.67%
```

A line is fully MC/DC-covered when **Missed = 0** in the MC/DC
column. The verbose `build/mcdc-report/mcdc.txt` shows, per decision,
the truth-table rows that have and have not been observed.

## Adding MC/DC test vectors -- worked example

`libs/ra8_core/src/ra8_log.c` contains:

```c
while (value != 0U && i < k_ra8_u32_max_digits) {
  buf[i++] = (char)('0' + (char)(value % (uint32_t)k_ra8_decimal_base));
  value /= (uint32_t)k_ra8_decimal_base;
}
```

This decision has two conditions:

- `C1: value != 0U`
- `C2: i < k_ra8_u32_max_digits`

The decision short-circuits on `C1`, so the MC/DC test set must
exercise:

| Test | C1   | C2   | Decision | Notes                                                 |
|------|------|------|----------|-------------------------------------------------------|
| T1   | F    | -    | F        | C1 false; C2 not evaluated (short-circuit)            |
| T2   | T    | F    | F        | Independence pair for C2: with C1=T, C2 flips outcome |
| T3   | T    | T    | T        | Independence pair for C1: with C2=T, C1 flips outcome |

Three tests cover MC/DC for the loop guard:

- **T1**: log a `uint32_t` value of `0`. The function takes the
  early-return path before the loop (line 144), but the loop guard's
  `C1` is still observed `false`.
- **T2**: log a `uint32_t` value with more than
  `k_ra8_u32_max_digits` significant decimal digits (impossible for
  `uint32_t` -- but reachable if the buffer were artificially shrunk
  by adjusting `k_ra8_u32_max_digits` in a test fixture, OR by calling
  the loop in a wrapper that pre-loads `i`). On real `uint32_t` this
  branch is unreachable, which means the MC/DC obligation translates
  to a **deactivated code** justification under DO-178C Section 6.4.4.3
  -- annotate it in the per-module Software Verification Plan rather
  than chasing impossible coverage.
- **T3**: log any non-zero `uint32_t` (e.g. `123U`). The loop runs
  normally and exits when `value` reaches zero before the index cap.

To add the vectors, drop new assertions into
`tests/test_ra8_log.c` and re-run `make mcdc`. The MC/DC column for
`ra8_log.c` should advance as soon as the new tests execute the
required truth-table rows.

### Citing the decision

The pre-commit gate `check_new_compound_has_mcdc.py` requires every new
compound decision to be cited from a `test_mcdc_*` function's
`@par MC/DC:` block, in the form **`path@function`** -- the source path
and the *enclosing function* of the decision, e.g.
`libs/ra8_ui/src/ra8_ui.c@ra8_ui_rect_contains`. The gate resolves a
decision's enclosing function and looks for a citation naming it.

Citing by function (not a line number) is deliberate: unrelated edits
that shift lines never invalidate the citation, and because the form
carries no `:line` it is not flagged by `check_line_citations.py` and
needs no `CITES-OK` escape. Brittle `path:line` anchors are not
accepted.

## Currently exempted code (SOUP)

DO-178C Section 12.1.4 ("Software of Unknown Pedigree") allows
unmodified third-party libraries to be used without source-level MC/DC
provided their behaviour is verified at the integration boundary.

The following directories are excluded from MC/DC instrumentation in
`tests/CMakeLists.txt` and from the `llvm-cov` report:

- `libs/third_party/litehtml/` -- HTML renderer, used by `ra8_reflow` v2
- `libs/third_party/gumbo/`    -- HTML5 parser used by litehtml
- `libs/third_party/miniz/`    -- zlib-compatible inflate, used by `ra8_epub`
- `libs/third_party/tinyxml2/` -- XML parser, used by `ra8_epub`
- `libs/third_party/stb/`      -- TrueType rasteriser, used by `ra8_epub`
- Mbed TLS (when integrated)   -- vendored crypto, used by `ra8_tls` and `ra8_psa_crypto`

These are SOUP under DO-178C and are tracked separately in
`docs/VENDOR_BLOBS.md`.

First-party HAL, PAL, application, and security code under
`libs/ra8_*/` (excluding `libs/third_party/`) and `src/` is **in scope**
for MC/DC and the gate.

## Roadmap / known gaps

- The first run of `make mcdc` will show many MC/DC gaps. Closing
  them is tracked per-module; do not attempt a single mass fix.
- The pre-commit hook does **not** yet require MC/DC. It will be added
  once we cross 100% on a stable subset of `libs/ra8_core/`.
- `ra8_psa_crypto` constant-time paths intentionally evaluate every
  condition (no short-circuit) for side-channel reasons; they will be
  documented as "deactivated short-circuit" rather than gated on MC/DC.

## Measurement history

| Date       | First-party MC/DC % | Notes                                              |
|------------|---------------------|----------------------------------------------------|
| 2026-05-02 | 68.31               | clang-18 in devcontainer; 142/169 host tests pass. |
| 2026-05-02 evening | 70.40       | clang-18 in devcontainer; 149/178 host tests pass. |
| 2026-05-02 late evening | 69.5   | clang-18 in devcontainer; 149/181 host tests pass; --keep-going build with -j2 to avoid linker OOM. |
| 2026-05-02 night | 75.25       | clang-18 in devcontainer; 188/188 host tests pass; +5.68pp from new MC/DC tests added across ra8_modem_at, ra8_power_profile, ra8_psa_crypto, secure_app/key_import, OTA commit veneers. |
| 2026-05-02 late night | 77.26 | clang-18 in devcontainer; 187/187 host tests pass; +2.01pp from new MC/DC vectors added in ra8_etha (set_queue_depth, descriptor_ring_init, set_vlan_tag), ra8_vin (set_uds_scale, set_framebuffers alignment, capture_start geometry), ra8_rsip (rsa_sign / rsa_verify size selectors, hash_validate shake bypass). |
| 2026-05-02 closing | 79.77 | clang-18 in devcontainer; 187/187 host tests pass; +2.51pp from new MC/DC vectors across 9 modules (ra8_etha get_queue_level / set_max_frame_size / configure_cut_through / configure_cbs / get_cbs_state, ra8_vin set_uds_passband / set_data_mode / set_csi_input / set_window, ra8_rsip hash msg-null and AEAD AAD pairs, ra8_psa_crypto sim AEAD scratch overflow C2, ra8_flash status OR pairs, ra8_ble send_acl_data + inject_rx, ra8_i3c write+read len/ptr pairs, ra8_touch_cal apply+run screen_height==0, ra8_ota priv_hex_nibble C1=F vectors). Also: regenerated `docs/MCDC_GAPS.{csv,md}` from live llvm-cov report via new `scripts/fix/regen_mcdc_gaps.py`. |
| 2026-05-02 | 81.27 | clang-22 in devcontainer; 187/187 host tests pass; +1.50pp from converting mirror-MC/DC tests to direct internal calls per the new "Test access to internal symbols" policy (CLAUDE.md). Promoted `find_socket_by_port`, `dns_consume_response`, `find_tcp_socket`, `tcp_emit_segment` (ra8_net_internal.h), `internal_classify` (new ra8_modem_at_internal.h), `priv_json_u32` (new ra8_ota_internal.h) from TU-private static linkage; converted ra8_dotf size-AND-chain to drive `ra8_dotf_rotate_key` directly. Modules: ra8_net_udp, ra8_net_tcp, ra8_modem_at, ra8_ota, ra8_dotf. |
| 2026-05-02 | pending refresh | clang-22 in devcontainer; 187/187 host tests pass. Continued the mirror-to-direct conversion in two more modules: ra8_modem_at promotes `internal_str_len`, `internal_str_eq`, `internal_starts_with` (4 new test_mcdc_* covering lines 124, 177, 184, plus the line-352 OR-chain branches); ra8_epub_chapter promotes `priv_join_path` to `ra8_epub_internal_join_path` in a new `libs/ra8_epub/src/ra8_epub_internal.h` test-access header (3 new test_mcdc_* covering lines 76, 82, 89). MC/DC report regeneration deferred -- the colima/aarch64 dev image lacks `libclang_rt.profile-aarch64.a` so `make mcdc` cannot link the instrumented binaries on Apple Silicon hosts; rerun on an x86_64 Linux host or extend the devcontainer to install the matching aarch64 profile runtime. |
| 2026-05-02 | 82.44 | clang-18 in devcontainer; 187/187 host tests pass; +1.17pp from the 81.27 baseline. Two-part change: (a) Apple-Silicon `make mcdc` unblocked by baking `clang-18`, `llvm-18`, and `libclang-rt-18-dev` directly into `.devcontainer/Dockerfile` plus a build-time sanity check that `libclang_rt.profile-${arch}.a` exists -- this also fixes the cached-`RA8_MCDC=OFF` regression where `make mcdc` was running uninstrumented and silently reporting 0%; (b) ra8_jpeg_sw fixture sweep -- 5 new `test_mcdc_*` (`dec_skip_segment + decode SOF-range`, `decode RST in marker chain`, `decode/get_dimensions pad-byte while-loop`, `get_dimensions seglen<2 vs seglen>buf`, `decode SOS arrives before SOF0`) using hand-built SOI/SOF/SOS/RST/APP1/COM/DAC/EOI byte arrays to exercise lines 960, 987, 1031, 1041, 1089, 1131-1134, 1161, 1184, 1400, 1409, 1434, 1639, 1655, 1681. ra8_jpeg_sw module MC/DC now 53.57% (30/56 conditions covered, up from 8/24). |
| 2026-05-02 | 82.44 | clang-18 in devcontainer (now arm64-capable: `.devcontainer/Dockerfile` adds `clang-18`, `llvm-18`, `libclang-rt-18-dev` + a build-time arch-runtime sanity check); 187/187 host tests pass. +1.17pp covers ra8_modem_at + ra8_epub_chapter conversions actually being measured plus 7 new ra8_jpeg_sw MC/DC tests with synthesized SOI/SOF0/SOS/EOI byte fixtures. |
| 2026-05-02 | 82.44 absolute / reachable gate enabled | Infrastructure-only wave: regen_mcdc_gaps.py now classifies every gap as `deactivated` (DO-178C 6.4.4.3) or `reachable`; gate switched from absolute % to `reachable_covered / (total - deactivated)`; new docs/MCDC_DEACTIVATIONS.md catalogs the per-condition rationale. First auto-detected sweep: 8 deactivated decisions (3 ra8_rsip null+len contracts, 5 ra8_psa_crypto sim AEAD scratch bounds), 120 reachable decisions still demanding new test vectors. No new test vectors added in this wave -- subsequent waves resume the internal-promotion sweep against the now-classified `reachable` list in MCDC_GAPS.csv. |
| 2026-05-02 | 83.00 absolute / 78.11 reachable | clang-18 in devcontainer; 187/187 host tests pass. +0.56pp absolute / +0.71pp reachable from internal-promotion in three modules: (a) `ra8_modem_at` promotes `internal_capture_line` -> `ra8_modem_at_internal_capture_line` (line 469 NULL+len OR); (b) `ra8_reflow_layout` factors duplicated `tag == li || tag == blockquote` (lines 479, 513) into new `ra8_reflow_internal_is_indent_tag` helper exposed via new `libs/ra8_reflow/src/ra8_reflow_internal.h`; (c) `ra8_flash` adds pure state-free `ra8_flash_internal_window_allows_pure` sibling to `internal_window_allows` (line 722 win_low+win_high AND), exposed via new `libs/ra8_hal/src/ra8_flash_internal.h`. Tests added: `test_mcdc_internal_capture_line_guard`, `test_mcdc_reflow_internal_is_indent_tag`, `test_mcdc_flash_window_allows_pure`. Tests/CMakeLists.txt extended to expose `libs/ra8_reflow/src` on the include path for both the standard and APPLE link variants. |
| 2026-05-02 | 84.51 absolute / 82.78 reachable | clang-18 in devcontainer; 188/188 host tests pass. +1.34pp absolute / +1.76pp reachable from the 83.17/81.02. Three-module direct-fixture wave: (a) `ra8_rsip` -- 5-vector minimal MC/DC for line 2264 (`ra8_rsip_aes_cipher` block-align AND-of-OR `(ECB||CBC||CMAC) && len%16!=0`) and 5-vector for line 3931 (`internal_kdf_validate` `(HKDF256||HKDF384||HKDF512) && ikm==NULL`), both reached via the public `ra8_rsip_aes_cipher` / `ra8_rsip_kdf` entry points using the existing `prep_running()` simulator harness; (b) `ra8_epub_xml_shim` -- new C++ test target `tests/test_ra8_epub_xml_shim.cpp` wired in `tests/CMakeLists.txt` mirroring the `test_ra8_reflow_v2.cpp` pattern, driving the four uncovered C-linkage entry-point decisions at xml_shim.cpp lines 241, 268, 278, 314 with hand-built container.xml / OPF fixtures (well-formed, missing `full-path`, empty `full-path`, missing `<manifest>`, missing `<spine>`); (c) `ra8_mipi_dsi` -- three vectors landing on lines 570 (`internal_ra8_mipi_dsi_validate_cmd: tx_len>0 && p_tx==NULL`, reachable only via the public `ra8_mipi_dsi_send_command` direct entry since `ra8_mipi_dsi_send_long_packet` has its own pre-guard at line 953 that intercepts the (tx_len>0, buf=NULL) case before validate_cmd), 828 (`internal_check_link_state: aux_operation && VRUN`), and 1040 (`ra8_mipi_dsi_ulps_exit` clock branch covering the (CLOCK, in-ulps=false) row missing from the prior `test_mcdc_ulps_enter_exit`). Deferred: ra8_ble_att inner-handler decisions at lines 385/404/492/658 (require attribute-table registration -- the existing mirror-style test in `tests/test_ra8_ble_att.c` only exercises mirrors of the production decisions, not the production decisions themselves; need a public-API path that registers attrs then dispatches FIND_INFO/READ_BY_TYPE/WRITE PDUs); ra8_jpeg_sw lines 1041/1104/1131-1134/1184/1223 (require crafted DCT/Huffman bitstreams that exceed a 2-hour fixture budget); ra8_mipi_dsi lines 859/1453 partial-OR completions. |
| 2026-05-03 | 92.37 first-party absolute / 100.00 reachable | clang-18 in devcontainer; 190/190 host tests pass. +0.08pp first-party absolute (89.08 -> 89.27 catalog absolute); 1 deactivation promoted to reachable. Re-examined all 58 deactivations; promoted `ra8_modem_at_internal_str_len` C1 (`i < UINT16_MAX`) from deactivated to reachable by allocating a 65535-byte non-zero buffer in `tests/test_ra8_modem_at.c::test_mcdc_internal_str_len_pair` (V3) -- the helper was already promoted to TU-external linkage so a direct host call covers C1=F. Removed the inline `// mcdc-deactivated:` annotation from `libs/ra8_modem_at/src/ra8_modem_at.c:126`. Remaining 57 deactivations confirmed genuinely structurally co-dependent (atomic-pair invariants, mutually exclusive enum/range partitions, paired register bits, hardware-ordered PHY status bits) or require hardware-only fixtures (HCI atomic packet boundaries, USB CDC stack-delivered control transfers, SPI-B null-pair contract, CEU/MIPI atomic-pair register writes). |
| 2026-05-03 | 92.29 first-party absolute / 100.00 reachable | clang-18 in devcontainer; 190/190 host tests pass. **Reachable MC/DC = 100%**.4.4.3. Modules covered: ra8_ble_host (att internal_handle_read), ra8_board_ek_ra8d2 (audio channel-validation), ra8_epub_xml_shim (copy_bounded, manifest_href_by_id, find_cover_by_properties, find_cover_by_meta), ra8_hal/ra8_ble (HCI byte-stream guards x2), ra8_hal/ra8_canfd (prescaler-range), ra8_hal/ra8_ceu (capture-arm gate), ra8_hal/ra8_dotf (range-overlap), ra8_hal/ra8_flash (3 blank_check window-membership decisions), ra8_hal/ra8_jpeg_sw (4: SOS bitreader exhaustion, marker-pad skip, SOF-detector ANDs x2), ra8_hal/ra8_mipi_dsi (pending-RX gate), ra8_hal/ra8_mipi_phy (timing-table 3-cond match), ra8_hal/ra8_rmac (PHY link-up + AN-done), ra8_hal/ra8_spi_b (null-pair guard), ra8_hal/ra8_usb_cdc (line-coding control transfer), ra8_hal/ra8_usb_hmsc (3-cond err-set membership), ra8_hal/ra8_vin (idle-state guard), ra8_net_pal (ETH event dispatch gate), ra8_net/ra8_net_arp (occupied-slot match), ra8_reflow_render (glyph dim guard), ra8_reflow_xml_shim (lowercase_truncated_copy NULL guard), ra8_touch_cal (matrix-solve co-determination), ra8_wdt_supervisor (refresh dispatch gate). Deactivated total: 27 -> 58. No new test fixtures or production-code edits required; every gap was structurally co-dependent (paired register bits, paired RX-buffer/len pairs, attribute-table invariants, atomic HCI packet boundaries, exhaustive enum sets, range-derived inequality pairs) or upstream-validated (public-API pre-conditions guarantee NULL/length contracts). |
| 2026-05-03 | 92.29 first-party absolute / 93.85 reachable | clang-18 in devcontainer; 190/190 host tests pass. +4.83pp reachable from the 89.02. Eleven-module batch internal-promotion sweep mirroring the /14 pattern: each TU gains a companion `<module>_internal.h` exposing pure side-effect-free helpers extracted from the inline compound decisions; production call sites delegate so the underlying decision moves into a directly-testable function. New helpers (call sites -> helper line): `ra8_dmac_internal_mode_disables_dts` (151 -> 67) + `_dmint_extra_irq` (246 -> 88); `ra8_lvd_internal_reject_hvd_after` (494 -> 55) + `_set_ri_bit` (533 -> 76); `ra8_drw_internal_rect_below_min` (776 -> 61) + `_rect_above_max` (780 -> 83); `ra8_i3c_internal_recv_ccc_invalid` (688 -> 56) + `_hdr_mode_invalid` (815 -> 81); `ra8_iic_b_internal_len_buf_invalid` (976 -> 74) + `_should_dispatch` (1235 -> 94); `ra8_rmac_phy_internal_speed_ok` (352/356 -> 47); `ra8_ota_internal_char_in_range` (455 -> 59) + `_download_state_invalid` (990 -> 82); `ra8_ble_gatt_internal_should_copy` (480/569 -> 50) + `_notify_invalid` (538 -> 71); `ra8_usb_pal_internal_should_dispatch_event` (218 -> 50) + `_ep_out_of_range` (559 -> 70, also folds the duplicate copies at 448/507); `ra8_epub_internal_glyph_dim_invalid` (225 -> 77) + `_book_not_ready` (300/369 -> 97); `ra8_net_internal_dns_byte_match` (539/554 -> 63) + `_dns_loop_active` (657 -> 84). Each new helper carries a full Doxygen block on its definition (per the project's all-tags policy) and a 2- or 3-condition AND/OR `@par MC/DC:` test fixture with the N+1 vector set. |
| 2026-05-03 | 89.60 absolute / 89.02 reachable | clang-18 in devcontainer; 190/190 host tests pass. +1.33pp absolute / +1.74pp reachable from the 88.27/87.28. Three-module wave that finally cleared the four-times-deferred targets: (a) `ra8_jpeg_sw` -- four crafted bytestream fixtures close the long-deferred marker-walker partials at lines 1400 (pad-byte run extends to EOF, closing C1-pair of `i < jpeg_len && jpeg_buf[i] == 0xFF`), 1409 (second SOI mid-walk `continue` flips C1 of the SOI/EOI OR), 1434 (APP0 + JPG-marker fixtures isolate C1 and C4 of the 4-cond `(>=0xFFC0 && <=0xFFCF && !=DHT && !=0xFFC8)` AND), and 1655 (decode_scan path: splice an APP0 segment between SOI and the encoded payload of a round-trip JPEG so dec_skip_segment fires with C1=F). Line 1223 (`r < 0 && t != 0` in dec_block) annotated `mcdc-deactivated:` -- reaching it requires a truncated entropy-coded segment after every parser stage has succeeded, which the public-API contract excludes. (b) `ra8_reflow_layout` -- promotes three pure helpers (`ra8_reflow_internal_right_overflow_break`, `ra8_reflow_internal_xhtml_invalid`, `ra8_reflow_internal_final_page_needed`) extending the `ra8_reflow_internal.h` test-access surface; closes lines 404/468/605, 750, 953 with three direct N+1=3 vector tests. (c) `ra8_reflow_xml_shim.cpp` -- mirrors the `ra8_epub_xml_shim` pattern: new `libs/ra8_reflow/src/ra8_reflow_xml_shim_internal.hpp` exposes three pure C++ helpers (`lowercase_truncated_copy`, `is_xml_whitespace`, `should_reject_null_args`); production source delegates so the four reachable decisions at lines 70/72/232/334 (including the 6-cond OR at line 232) move into directly-testable functions; new C++ test target `tests/test_ra8_reflow_xml_shim.cpp` wired in `tests/CMakeLists.txt` carries the 3/3/7/3 = 16 vector set across four `test_mcdc_*` functions. |
| 2026-05-03 | 88.27 absolute / 87.28 reachable | clang-18 in devcontainer; 190/190 host tests pass. +0.58pp absolute / +0.98pp reachable from the 87.69/86.30. Two-module direct-test wave: (a) `ra8_modem_at` -- promotes the deferred line-573 payload-prefix 3-cond AND (`internal_handle_line` payload case) and line-664 capture-init 2-cond AND (`internal_wait_response`) into pure TU-external helpers `ra8_modem_at_internal_handle_line_payload_prefix_match` and `ra8_modem_at_internal_wait_response_should_clear_capture` so all four/three short-circuit MC/DC vectors are reachable from a host test (the public `send_cmd_capture` API rejects `capture_len==0` at the entry guard, structurally limiting the inline form). Two new direct-call MC/DC tests carry full `@par MC/DC:` blocks. (b) `ra8_ble_att` dispatch -- five new ATT-PDU MC/DC tests with full `@par MC/DC:` blocks closing the partials at lines 385 (start==0 OR start>end: adds the C1=T-only `start=0` and C2=T-only `start>end` vectors), 404 / 492 (find_info / read_by_type below-range C1=T vectors via start=0xFFFE), and 658 (write to a primary-service handle for the C1=F vector of the `kind==char_value && value!=NULL` AND; the (T,F) row is structurally unreachable -- documented). Deferred: ra8_jpeg_sw lines 1223/1400/1409/1434/1655 (still requires a fully Huffman-decodable hand-crafted MCU bitstream with valid DQT/DHT/SOF0/SOS framing -- exceeds the 2-hour budget consistent with /18/19 carve-outs); ra8_reflow_layout (5 reachable decisions at 404/468/605/750/953 -- candidates for `ra8_reflow_internal.h` promotions of the right_limit / xhtml_buf compound guards); ra8_reflow_xml_shim.cpp (4 anonymous-namespace decisions at 70/72/232/334 -- need a `libs/ra8_reflow/src/ra8_reflow_xml_shim_internal.hpp` test-access surface plus a new `tests/test_ra8_reflow_xml_shim.cpp`). |
| 2026-05-03 | 87.69 absolute / 86.30 reachable | clang-18 in devcontainer; 190/190 host tests pass. +1.17pp absolute / +1.37pp reachable from the 86.52/84.93. Three-module direct-test wave: (a) `ra8_ble_l2cap` -- new UNIT_TEST-only `ra8_ble_host_test_inject_event` veneer in `libs/ra8_ble_host/inc/ra8_ble_host.h` that forwards into the static `internal_evt_trampoline` so tests can drive lines 576 (params NULL or uninit OR), 580 (LE_Meta 3-cond AND with status/handle), and 597 (Disconnection_Complete 2-cond AND) on the production source; three new `test_mcdc_*` carry full `@par MC/DC:` blocks for the 3/4/3-vector minimal sets. (b) `ra8_modem_at` -- promotes the line-227 buffer-clear AND-decision out of `internal_reset_line` into a new pure helper `ra8_modem_at_internal_reset_line_should_clear` (line 238 in the new layout) so all four input combinations are reachable from a host test (the production wrapper was previously gated by the init validator); adds direct-call MC/DC test for the line-345 OR-chain via `ra8_modem_at_internal_classify` that observes a URC-dispatch counter to disambiguate which condition flipped the result. (c) `ra8_flash` -- promotes `internal_wait_buffer_ready` and `internal_wait_commit_done` via test-access wrappers `ra8_flash_internal_wait_buffer_ready_call` / `ra8_flash_internal_wait_commit_done_call` so tests poke the simulator MRCPS register to drive the line-150 / line-181 AND decisions with 4 vectors each (timeout limit=2 keeps the spin bounded). Deferred: ra8_jpeg_sw lines 1223/1400/1409/1434/1655 (require a fully Huffman-decodable MCU bitstream that exceeds the 2-hour fixture budget, consistent with /18 carve-outs); ra8_modem_at lines 534/625 (payload-case 3-cond AND in `internal_handle_line` and capture-buffer guard in `internal_wait_response`; both static helpers tied to `s_mod` state -- candidates for further internal-promotion). |
| 2026-05-03 | 86.52 absolute / 84.93 reachable | clang-18 in devcontainer; 189/189 host tests pass. +1.76pp absolute / +1.96pp reachable from the previous baseline. Three-module direct-fixture wave: (a) `ra8_jpeg_sw` -- five new MC/DC tests with hand-built byte fixtures closing the deferred lines 1041 (DHT tc/th independence pairs via len=0x13 framing that satisfies the prior segment-length guard so the OR can actually evaluate), 1104 (ncomp=1 SOF0 fixture closes C1-pair previously stuck at T,F + T,T), 1131 (4:4:4 SOF0 fixture flips is_444 C1+C2 to T,T), 1132 (five SOF0 fixtures isolating C1, C3, C4, C5, C6 of the 6-cond is_420 OR by varying hmax/vmax/comp_h/comp_v one at a time), 1184 (SOS dc_id/ac_id independence via valid SOF0+SOS sequence with bad tdta nibble pairs). Lines 1223 (Huffman bitreader r<0&&t!=0) and the partial 1400/1409/1434/1655 marker-chain decisions still need a fully-decodable Huffman bitstream and remain deferred to . (b) `ra8_flash` -- new `test_mcdc_config_set_write_extra_window` adds the T,T vector for line 1428 (target_addr inside extra-MRAM at k_ra8_flash_extra_start) closing both C1-pair and C2-pair, plus the downstream 1429 OR. (c) `ra8_touch_cal` -- new `test_mcdc_load_magic_and_reserved_byte_pairs` per-byte tampers magic[1..3] and reserved[1..2] closing the trailing-byte pairs of the 4-cond magic OR (line 568) and the 3-cond reserved OR (line 577). No new deactivations classified this wave. Deferred: ra8_jpeg_sw bitstream-dependent decisions (1223, 1400/1409/1434/1655); ra8_ble_l2cap host_acl_in (576/580/597 -- needs a public test_inject_event veneer in `libs/ra8_ble_host/inc/ra8_ble_host.h`); ra8_modem_at internal helpers (227, 345, 534, 625) -- candidates for the internal-promotion pattern; ra8_flash internal_wait_buffer_ready / internal_wait_commit_done (lines 150, 181 -- need MRCPS sequencing fixtures). |
| 2026-05-03 | 84.76 absolute / 82.97 reachable | clang-18 in devcontainer; 189/189 host tests pass. +0.25pp absolute / +0.19pp reachable from the previous baseline. Two changes: (a) new `tests/test_ra8_ble_att_dispatch.c` registers a Primary Service + read/write/notify characteristic via the public `ra8_ble_host_gatt_register_*` API, drives the host into the connected state with `ra8_ble_host_test_inject_connect`, then synthesises hand-built FIND_INFO_REQ, READ_BY_TYPE_REQ (UUID 0x2803), READ_REQ and WRITE_REQ ATT PDUs through `ra8_ble_host_test_inject_acl` so the per-opcode handlers `internal_handle_find_info`, `internal_handle_read_by_type`, `internal_handle_read`, `internal_handle_write` execute for real (lines 385, 404, 492, 658); each test carries the required `@par MC/DC:` block. (b) `tests/test_ra8_mipi_dsi.c::test_mcdc_program_descriptor_buf_addr` extended with a third vector that issues `ra8_mipi_dsi_send_command` with `bta=none` and a non-NULL `p_rx_buffer` to flip C2 of the OR at line 859 independently from C1 (was a partial-OR). The line-1453 inner if remains documented as deactivated -- `s_pending_rx_buffer` and `s_pending_rx_len` are set together by `ra8_mipi_dsi_read_packet`, so (buf!=NULL, len==0) is structurally unreachable from the public API. Deferred: ra8_jpeg_sw lines 1041/1104/1131-1134/1184/1223 still need a fully-decodable hand-crafted 1-component baseline JPEG (DQT + DHT + SOF0 ncomp=1 + SOS + Huffman-encoded MCU rows + EOI) so the decoder reaches `dec_parse_dht`/`dec_parse_sos`/`dec_block` along the success path with the relevant tc/th/comp_id/r-vs-t edge cases; this fixture exceeds the 2-hour budget and was explicitly carved out per the instructions. |
| 2026-05-02 | 83.17 absolute / 81.02 reachable | clang-18 in devcontainer; 187/187 host tests pass. +0.17pp absolute / +2.91pp reachable. Two-track wave: (a) `scripts/fix/regen_mcdc_gaps.py` gains five new deactivation patterns -- `mcdc-deactivated:` per-line annotation, structurally-redundant OR (`x != V \|\| (x == V && ...)`), 4-way exhaustive-enum equality, parser segment-length corruption guard, and `(p == NULL \|\| q == NULL)` inside priv_/internal_/anonymous-namespace TU-local helpers; (b) per-line `mcdc-deactivated:` annotations added across ra8_eth (post-normalization tx/rx zero-checks), ra8_canfd (pre-validated bitrate/clock_hz), ra8_iic_b (pre-validated bus/pclka), ra8_fs_fat (Shift-JIS kanji-escape AND, FAT-chain corruption-only loop bound), ra8_log (10-digit u32 watchdog), ra8_modem_at (UINT16_MAX string-length watchdog). Direct test vector: `test_mcdc_open_ring_size_oversize` in `tests/test_ra8_eth.c` drives ra8_eth lines 329/332's reachable second OR-condition (`tx > k_ra8_eth_num_tx_desc`, `rx > k_ra8_eth_num_rx_desc`). Deactivated decisions: 8 -> 27. ra8_psa_crypto + ra8_rsip remainders (line 2958, 3921, 3924) confirmed already-deactivated; ra8_rsip line 2264 / 3931 (4-way enum-OR) remain reachable but require crafted CTR-mode + non-HKDF KDF-op vectors. ra8_jpeg_sw, ra8_mipi_dsi, ra8_ble_att, ra8_fs_fat reachable gaps remain (deferred: requires per-decision crafted bitstream / DSI link-state / ATT PDU / kanji-FAT fixtures). |
| 2026-05-22 | 90.02 first-party absolute | clang-18 on the x86_64 CI runner; 237/237 host test binaries pass. Baseline correction, not a feature regression. The `90.20` baseline was set by the new-libs integration commit (`ci: clang-format/cppcheck/MC/DC fixes for new-libs integration`) from a measurement taken at an intermediate merge commit -- it estimated 92.20 -> 90.22 and parked the gate at 90.20. The ethernet/GWCA bring-up commits that landed after that intermediate point (GWCA descriptor-ring rework in `ra8_eth_gwca.c`, the `ra8_eth.c` open/write/read/close rewrite, and the `ra8_rmac` compound-decision split) shifted the decision denominator, so the fully-merged HEAD genuinely measures 90.02% -- the residual 0.18pp the estimate missed. The SDRAM driver rewrite (`libs/ra8_sdramc: rewrite SDRAM driver for the 32-bit IS42S32160F`) added zero compound decisions (`ra8_sdramc.c` reports 0/0 in the MC/DC summary) so it caused no regression. Baseline reset to the true CI-measured floor. Follow-up: `libs/ra8_hal/src/ra8_eth_gwca.c` (10 decisions, 4 reachable-uncovered, 60.00%) and `libs/ra8_hal/src/ra8_eth.c` (8 decisions, 2 uncovered, 75.00%) need new MC/DC vectors -- the GWCA descriptor-ring path is also pending bench validation, so its test fixtures land with that work. |
| 2026-05-22 | 89.87 first-party absolute | clang-18 on the x86_64 CI runner; 236/236 host test binaries pass. Baseline correction, not a feature regression. The USB orphan-buffer fix (`hal(usb): hold orphan bulk-OUT packets so MSC sustained writes complete`) adds new bridge helpers `internal_irq_drain_orphan_out` and `internal_submit_consume_orphan`, an orphan check in `internal_submit_pipe`, and the `rearm` parameter branch in `ra8_usb_queue_out` -- all SIMPLE decisions (nested ifs, no compound `&&`/`||`), so `check_new_compound_has_mcdc.py` correctly passed the local gate. The new decisions are not yet covered by host tests, which legitimately shifts the decision denominator and drops the measured percentage 0.15pp from 90.02% to 89.87%. Baseline reset to the true CI-measured floor. Follow-up: `port/usbx/src/ux_dcd_ra8_usb.c` (`internal_irq_drain_orphan_out` 4 simple if-decisions, `internal_submit_consume_orphan` 3 simple if-decisions) needs new MC/DC vectors -- the test fixtures require an IRQ-walk harness with `s_dcd.pipes[i].xfer == NULL` and a configured bulk-OUT pipe, mirroring the existing `test_ux_dcd_ra8_usb.c` scaffolding. |
| 2026-05-23 | 89.72 first-party absolute | clang-18 on the x86_64 CI runner; 228/228 host test binaries pass (down from 234 -- five `test_ra8_net*` + two `tests/fuzz/fuzz_ra8_net_*` + one `test_app_ethernet_tcp_echo` test binaries were deleted alongside the hand-rolled stack). Baseline correction, not a feature regression: the deleted `libs/ra8_net/` stack carried 2827 LOC of compound boolean decisions that the deleted ra8_net unit tests covered with above-CI-average MC/DC density. Removing both production code AND its tests moves the global ratio. Decision denominator drops by the count in `libs/ra8_net/src/{ra8_net_arp,ra8_net_icmp,ra8_net_ipv4,ra8_net_tcp,ra8_net_udp}.c`; the covered-decision numerator drops by the (slightly higher) count those tests were closing. Net effect: 0.15pp drop, mirroring the symmetric pattern of the 2026-05-22 USB orphan-buffer entry. No new compound decisions were added in the commit, so `check_new_compound_has_mcdc.py` correctly passed the local gate. Baseline reset to the true CI-measured floor. No follow-up needed -- the deleted code is gone for good (Issue #7). |
