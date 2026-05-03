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
- Decisions at 100% MC/DC (`yes`): **394**
- Decisions partially covered (`partial`): **71**
- Decisions fully uncovered (`no`): **74**
- Coverage rate (yes / total): **73.1%**

## Per-module gap counts (full table)

Sorted by (uncovered + partial) descending, then total descending.

| Module | Total | Covered | Partial | Uncovered |
|--------|------:|--------:|--------:|----------:|
| ra_jpeg_sw | 24 | 6 | 10 | 8 |
| ra_epub_xml_shim | 12 | 0 | 3 | 9 |
| ra_flash | 20 | 12 | 3 | 5 |
| ra_modem_at | 12 | 4 | 6 | 2 |
| ra_reflow_layout | 14 | 7 | 4 | 3 |
| ra_net_udp | 9 | 2 | 1 | 6 |
| ra_epub_chapter | 13 | 7 | 4 | 2 |
| ra_mipi_dsi | 22 | 17 | 3 | 2 |
| ra_psa_crypto | 21 | 16 | 5 | 0 |
| ra_rsip | 16 | 11 | 2 | 3 |
| ra_fs_fat | 20 | 16 | 1 | 3 |
| ra_ota | 8 | 4 | 3 | 1 |
| ra_ble_att | 5 | 1 | 0 | 4 |
| ra_reflow_xml_shim | 4 | 0 | 2 | 2 |
| ra_touch_cal | 13 | 10 | 3 | 0 |
| ra_ble_l2cap | 12 | 9 | 0 | 3 |
| ra_ble_gatt | 11 | 8 | 3 | 0 |
| ra_iic_b | 7 | 4 | 2 | 1 |
| ra_net_tcp | 3 | 0 | 0 | 3 |
| ra_ble | 8 | 6 | 0 | 2 |
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
| ra_jpeg_sw | 8 | 10 | 6 | 24 |
| ra_net_udp | 6 | 1 | 2 | 9 |
| ra_flash | 5 | 3 | 12 | 20 |
| ra_ble_att | 4 | 0 | 1 | 5 |
| ra_reflow_layout | 3 | 4 | 7 | 14 |
| ra_rsip | 3 | 2 | 11 | 16 |
| ra_fs_fat | 3 | 1 | 16 | 20 |
| ra_ble_l2cap | 3 | 0 | 9 | 12 |
| ra_net_tcp | 3 | 0 | 0 | 3 |
| ra_modem_at | 2 | 6 | 4 | 12 |
| ra_epub_chapter | 2 | 4 | 7 | 13 |
| ra_mipi_dsi | 2 | 3 | 17 | 22 |
| ra_reflow_xml_shim | 2 | 2 | 0 | 4 |
| ra_ble | 2 | 0 | 6 | 8 |
| ra_eth | 2 | 0 | 2 | 4 |
| ra_rmac_phy | 2 | 0 | 1 | 3 |
| ra_ota | 1 | 3 | 4 | 8 |
| ra_iic_b | 1 | 2 | 4 | 7 |
| ra_dmac | 1 | 1 | 1 | 3 |
| ra_i3c | 1 | 1 | 5 | 7 |
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
