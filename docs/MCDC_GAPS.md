# MC/DC Coverage Gap Audit

Static audit of compound boolean decisions across first-party
sources in `libs/`, `src/`, and `port/` (excluding third-party).
Generated as a one-shot static pass (no coverage tools were
run; a sibling agent is wiring those up).

## Methodology

- A *compound decision* is any expression whose top-level chain
  contains 2+ conditions joined by `&&` or `||`.
- *Condition count N* implies N+1 distinct test vectors are
  required to satisfy MC/DC.
- `suspected_mcdc_gap=yes` when either (a) no matching
  `tests/test_<module>*.c` exists, or (b) the decision has 3+
  conditions (unlikely to be covered by chance without targeted
  tests).
- `unknown` otherwise -- a sibling agent will replace these
  with measured coverage results.

## Top-line Numbers

- Total compound decisions found: **609**
- Total estimated MC/DC test vectors needed (sum of N+1): **1956**
- Source files with at least one compound decision: **106**

### Decisions by priority

| Priority | Decisions |
|---------:|----------:|
| high | 31 |
| medium | 577 |
| low | 1 |

## Top 10 Modules by Compound-Decision Count

| Module | Decisions | Estimated MC/DC vectors |
|--------|----------:|------------------------:|
| ra_jpeg_sw | 24 | 80 |
| ra_mipi_dsi | 22 | 68 |
| ra_mipi_phy | 22 | 67 |
| ra_psa_crypto | 21 | 72 |
| ra_fs_fat | 20 | 67 |
| ra_flash | 20 | 61 |
| ra_rsip | 17 | 60 |
| ra_etha | 16 | 57 |
| sys_arch | 15 | 45 |
| ra_vin | 14 | 49 |

## Top 10 Highest-Priority Modules by Gap Count

(Limited to modules tagged *high* priority -- see CSV column)

| Module | Decisions | Estimated MC/DC vectors |
|--------|----------:|------------------------:|
| ra_usb | 11 | 34 |
| ra_sci | 8 | 25 |
| ra_mpu | 7 | 21 |
| ra_xspi | 4 | 12 |
| ra_isr | 1 | 3 |

## Per-Module Gap Counts (full table)

Modules sorted by decision count (descending). Truncated rows
kept under the 200-line cap; the full data lives in
`docs/MCDC_GAPS.csv`.

| Module | Decisions | Vectors | Has tests |
|--------|----------:|--------:|:---------:|
| ra_jpeg_sw | 24 | 80 | yes |
| ra_mipi_dsi | 22 | 68 | yes |
| ra_mipi_phy | 22 | 67 | yes |
| ra_psa_crypto | 21 | 72 | yes |
| ra_fs_fat | 20 | 67 | no |
| ra_flash | 20 | 61 | yes |
| ra_rsip | 17 | 60 | yes |
| ra_etha | 16 | 57 | yes |
| sys_arch | 15 | 45 | no |
| ra_vin | 14 | 49 | yes |
| ra_reflow_layout | 14 | 43 | no |
| ra_epub_chapter | 13 | 43 | no |
| ra_touch_cal | 13 | 43 | yes |
| ra_ble_l2cap | 12 | 39 | no |
| ra_gfx_text | 12 | 42 | no |
| ra_modem_at | 12 | 38 | yes |
| ra_ble_gatt | 11 | 34 | yes |
| ra_usb | 11 | 34 | yes |
| ra_usb_paud | 11 | 39 | yes |
| ra_ssie | 10 | 34 | yes |
| ra_spi_b | 9 | 27 | no |
| ra_net_udp | 9 | 29 | no |
| ra_ota | 9 | 28 | yes |
| ra_ble | 8 | 26 | yes |
| ra_pdg | 8 | 24 | yes |
| ra_sci | 8 | 25 | yes |
| ra_usb_phid | 8 | 27 | yes |
| ra_usb_pal | 8 | 26 | yes |
| mbedtls_aes_alt | 8 | 28 | no |
| ra_drw | 7 | 21 | yes |
| ra_i3c | 7 | 22 | yes |
| ra_iic_b | 7 | 21 | yes |
| ra_mpu | 7 | 21 | yes |
| ra_tls | 7 | 21 | yes |
| ra_ble_gatt_client | 6 | 21 | yes |
| ra_dotf | 6 | 22 | yes |
| ra_usb_pprn | 6 | 18 | yes |
| ra_usb_pvnd | 6 | 21 | yes |
| ra_ble_att | 5 | 15 | no |
| ra_lvd | 5 | 15 | yes |
| ra_usb_haud | 5 | 15 | yes |
| ra_usb_hcdc_ecm | 5 | 15 | yes |
| ra_net_ipv4 | 5 | 17 | no |
| ra_canfd | 4 | 12 | yes |
| ra_eth | 4 | 12 | yes |
| ra_usb_cdc | 4 | 12 | yes |
| ra_usb_hhid | 4 | 13 | yes |
| ra_usb_hmsc | 4 | 14 | yes |
| ra_xspi | 4 | 12 | yes |
| gx_display_driver_ra_glcdc | 4 | 12 | no |
| ra_etha_netif | 4 | 12 | no |
| mbedtls_sha256_alt | 4 | 13 | no |
| nx_crypto_aes_alt | 4 | 14 | no |
| nx_ether_driver_ra_eth | 4 | 13 | no |
| ux_dcd_ra_usb | 4 | 12 | no |
| ux_hcd_ra_usb | 4 | 12 | no |
| ra_epub_open | 3 | 9 | no |
| ra_ceu | 3 | 9 | yes |
| ra_dmac | 3 | 9 | yes |
| ra_gpt | 3 | 9 | yes |
| ra_rmac_phy | 3 | 9 | yes |
| ra_net_tcp | 3 | 9 | no |
| ble_hci_ra_ble | 3 | 9 | no |
| nimble_npl_threadx | 3 | 9 | no |
| ra_ble_mesh | 2 | 6 | yes |
| ra_ble_security | 2 | 6 | yes |
| ra_board_ek_ra8d2 | 2 | 6 | yes |
| adc | 2 | 6 | no |
| ra_bkup | 2 | 6 | yes |
| ra_ble_patch | 2 | 6 | yes |
| ra_epaper | 2 | 11 | yes |
| ra_iic_b_slave | 2 | 6 | yes |
| ra_ipc | 2 | 6 | yes |
| ra_rmac | 2 | 6 | yes |
| ra_touch | 2 | 6 | yes |
| ra_tsn | 2 | 6 | yes |
| ra_usb_composite | 2 | 7 | yes |
| ra_usb_hcdc | 2 | 6 | yes |
| ra_usb_hhub | 2 | 6 | yes |
| ra_vreg | 2 | 6 | yes |
| ra_net_pal | 2 | 6 | yes |
| ra_wdt_supervisor | 2 | 6 | no |
| lx_filex_adapter | 2 | 6 | no |
| lx_nor_driver_ra_xspi | 2 | 8 | no |
| nx_crypto_sha256_alt | 2 | 6 | no |
| secure_trng | 2 | 6 | no |
| ra_log | 1 | 3 | yes |
| ra_crc | 1 | 3 | yes |
| ra_ether_phy | 1 | 3 | yes |
| ra_glcdc | 1 | 3 | yes |
| ra_isr | 1 | 3 | yes |
| ra_mipi_csi | 1 | 3 | yes |
| ra_ofs | 1 | 4 | yes |
| ra_ptp | 1 | 3 | yes |
| ra_pwr | 1 | 3 | yes |
| ra_smbus | 1 | 3 | yes |
| ra_sram | 1 | 3 | yes |
| ra_usb_pmsc | 1 | 3 | yes |
| ra_net_arp | 1 | 3 | no |
| ra_nsc_eth | 1 | 3 | no |
| ra_nsc_ota | 1 | 3 | no |
| ra_nsc_xspi | 1 | 3 | no |
| ra_reflow_parse | 1 | 3 | no |
| ra_reflow_render | 1 | 3 | no |
| key_import | 1 | 3 | no |
| ota_commit | 1 | 3 | no |

---

*Generated statically; replace `unknown` rows in the CSV with
real coverage data once `gcov`/`lcov` runs land.*
