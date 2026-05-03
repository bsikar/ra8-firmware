# MC/DC Coverage Gap Audit

Static audit of compound boolean decisions across first-party
sources in `libs/`, `src/`, and `port/` (excluding
`libs/third_party/`). Refreshed against the current test suite to
mark which decisions already have `test_mcdc_*` coverage.

## Methodology

- A *compound decision* is any source line containing 2+ logical
  operators (`&&` / `||`) after stripping comments and string
  literals. Condition count = operators + 1.
- The enclosing function is determined by walking matched
  brace ranges; the innermost wrapper wins.
- Coverage status (`covered` column):
  - `yes` -- either (a) some `tests/test_*.c` cites the exact
    `<source>.c line N` inside an `@par MC/DC:` block, OR (b) a
    `test_mcdc_*` function in the matching module test file has
    a name containing the enclosing function name (with the
    `internal_`/`priv_`/`ra_` prefix optionally stripped).
  - `partial` -- the module's test file has at least one
    `test_mcdc_*` function but none reference this decision.
  - `no` -- no matching `test_mcdc_*` function or line cite.

## Top-line Numbers

- Source files scanned: **158**
- Total compound decisions found: **633**
- Decisions with at least one MC/DC test (`yes`): **170**
- Decisions in a partially-covered module (`partial`): **395**
- Decisions with no MC/DC test (`no`, gap): **68**
- Coverage rate (yes / total): **26.9%**

## Per-module gap counts (full table)

Sorted by uncovered+partial count (descending).

| Module | Total | Covered | Partial | Uncovered |
|--------|------:|--------:|--------:|----------:|
| ra_rsip | 21 | 5 | 16 | 0 |
| ra_vin | 16 | 0 | 16 | 0 |
| ra_jpeg_sw | 25 | 10 | 15 | 0 |
| ra_etha | 20 | 5 | 15 | 0 |
| sys_arch | 15 | 0 | 0 | 15 |
| ra_mipi_dsi | 24 | 10 | 14 | 0 |
| ra_fs_fat | 20 | 6 | 14 | 0 |
| ra_flash | 20 | 7 | 13 | 0 |
| ra_touch_cal | 16 | 3 | 13 | 0 |
| ra_ble_l2cap | 13 | 0 | 13 | 0 |
| ra_ble_gatt | 12 | 0 | 12 | 0 |
| ra_psa_crypto | 21 | 10 | 11 | 0 |
| ra_reflow_layout | 14 | 3 | 11 | 0 |
| ra_usb_paud | 11 | 0 | 11 | 0 |
| ra_epub_chapter | 13 | 3 | 10 | 0 |
| ra_gfx_text | 12 | 3 | 9 | 0 |
| ra_ble | 9 | 0 | 9 | 0 |
| ra_spi_b | 9 | 0 | 0 | 9 |
| ra_modem_at | 12 | 4 | 8 | 0 |
| ra_net_udp | 9 | 1 | 8 | 0 |
| mbedtls_aes_alt | 8 | 0 | 0 | 8 |
| ra_usb_phid | 8 | 0 | 8 | 0 |
| ra_mipi_phy | 22 | 15 | 7 | 0 |
| ra_ssie | 10 | 3 | 7 | 0 |
| ra_dotf | 7 | 0 | 7 | 0 |
| ra_drw | 7 | 0 | 7 | 0 |
| ra_i3c | 7 | 0 | 7 | 0 |
| ra_iic_b | 7 | 0 | 7 | 0 |
| ra_ble_gatt_client | 6 | 0 | 6 | 0 |
| ra_usb_pprn | 6 | 0 | 6 | 0 |
| ra_usb_pvnd | 6 | 0 | 6 | 0 |
| ra_ota | 9 | 4 | 5 | 0 |
| ra_pdg | 8 | 3 | 5 | 0 |
| ra_ble_att | 5 | 0 | 5 | 0 |
| ra_lvd | 5 | 0 | 5 | 0 |
| ra_usb_haud | 5 | 0 | 5 | 0 |
| ra_usb_hcdc_ecm | 5 | 0 | 5 | 0 |
| ra_tls | 7 | 3 | 4 | 0 |
| gx_display_driver_ra_glcdc | 4 | 0 | 0 | 4 |
| mbedtls_sha256_alt | 4 | 0 | 0 | 4 |
| nx_crypto_aes_alt | 4 | 0 | 0 | 4 |
| nx_ether_driver_ra_eth | 4 | 0 | 0 | 4 |
| ra_epaper | 4 | 0 | 4 | 0 |
| ra_etha_netif | 4 | 0 | 0 | 4 |
| ra_usb_cdc | 4 | 0 | 4 | 0 |
| ra_usb_hhid | 4 | 0 | 4 | 0 |
| ux_hcd_ra_usb | 4 | 0 | 4 | 0 |
| ra_usb_pal | 8 | 5 | 3 | 0 |
| ra_usb_hmsc | 5 | 2 | 3 | 0 |
| ux_dcd_ra_usb | 4 | 1 | 3 | 0 |
| ble_hci_ra_ble | 3 | 0 | 0 | 3 |
| nimble_npl_threadx | 3 | 0 | 0 | 3 |
| ra_xspi | 4 | 2 | 2 | 0 |
| ra_ceu | 3 | 1 | 2 | 0 |
| ra_dmac | 3 | 1 | 2 | 0 |
| ra_rmac_phy | 3 | 1 | 2 | 0 |
| lx_filex_adapter | 2 | 0 | 0 | 2 |
| nx_crypto_sha256_alt | 2 | 0 | 0 | 2 |
| ra_bkup | 2 | 0 | 2 | 0 |
| ra_ble_patch | 2 | 0 | 2 | 0 |
| ra_ble_security | 2 | 0 | 2 | 0 |
| ra_board_ek_ra8d2 | 2 | 0 | 0 | 2 |
| ra_iic_b_slave | 2 | 0 | 2 | 0 |
| ra_ipc | 2 | 0 | 2 | 0 |
| ra_rmac | 2 | 0 | 2 | 0 |
| ra_touch | 2 | 0 | 2 | 0 |
| ra_tsn | 2 | 0 | 2 | 0 |
| ra_usb_composite | 2 | 0 | 2 | 0 |
| ra_usb_hcdc | 2 | 0 | 2 | 0 |
| ra_usb_hhub | 2 | 0 | 2 | 0 |
| ra_vreg | 2 | 0 | 2 | 0 |
| ra_wdt_supervisor | 2 | 0 | 2 | 0 |
| secure_trng | 2 | 0 | 0 | 2 |
| ra_sci | 8 | 7 | 1 | 0 |
| ra_canfd | 4 | 3 | 1 | 0 |
| lx_nor_driver_ra_xspi | 3 | 2 | 1 | 0 |
| ra_epub_open | 3 | 2 | 1 | 0 |
| ra_gpt | 3 | 2 | 1 | 0 |
| ra_ble_mesh | 2 | 1 | 1 | 0 |
| key_import | 1 | 0 | 0 | 1 |
| ota_commit | 1 | 0 | 0 | 1 |
| ra_ether_phy | 1 | 0 | 1 | 0 |
| ra_mipi_csi | 1 | 0 | 1 | 0 |
| ra_nsc_eth | 1 | 0 | 1 | 0 |
| ra_nsc_xspi | 1 | 0 | 1 | 0 |
| ra_ofs | 1 | 0 | 1 | 0 |
| ra_ptp | 1 | 0 | 1 | 0 |
| ra_pwr | 1 | 0 | 1 | 0 |
| ra_smbus | 1 | 0 | 1 | 0 |
| ra_sram | 1 | 0 | 1 | 0 |
| ra_usb_pmsc | 1 | 0 | 1 | 0 |
| ra_usb | 11 | 11 | 0 | 0 |
| ra_mpu | 7 | 7 | 0 | 0 |
| ra_net_ipv4 | 5 | 5 | 0 | 0 |
| ra_eth | 4 | 4 | 0 | 0 |
| ra_net_tcp | 3 | 3 | 0 | 0 |
| adc | 2 | 2 | 0 | 0 |
| ra_net_pal | 2 | 2 | 0 | 0 |
| ra_crc | 1 | 1 | 0 | 0 |
| ra_glcdc | 1 | 1 | 0 | 0 |
| ra_isr | 1 | 1 | 0 | 0 |
| ra_log | 1 | 1 | 0 | 0 |
| ra_net_arp | 1 | 1 | 0 | 0 |
| ra_nsc_ota | 1 | 1 | 0 | 0 |
| ra_reflow_parse | 1 | 1 | 0 | 0 |
| ra_reflow_render | 1 | 1 | 0 | 0 |

## Top 30 modules with at least one uncovered decision

| Module | Uncovered | Partial | Covered | Total |
|--------|----------:|--------:|--------:|------:|
| sys_arch | 15 | 0 | 0 | 15 |
| ra_spi_b | 9 | 0 | 0 | 9 |
| mbedtls_aes_alt | 8 | 0 | 0 | 8 |
| gx_display_driver_ra_glcdc | 4 | 0 | 0 | 4 |
| mbedtls_sha256_alt | 4 | 0 | 0 | 4 |
| nx_crypto_aes_alt | 4 | 0 | 0 | 4 |
| nx_ether_driver_ra_eth | 4 | 0 | 0 | 4 |
| ra_etha_netif | 4 | 0 | 0 | 4 |
| ble_hci_ra_ble | 3 | 0 | 0 | 3 |
| nimble_npl_threadx | 3 | 0 | 0 | 3 |
| lx_filex_adapter | 2 | 0 | 0 | 2 |
| nx_crypto_sha256_alt | 2 | 0 | 0 | 2 |
| ra_board_ek_ra8d2 | 2 | 0 | 0 | 2 |
| secure_trng | 2 | 0 | 0 | 2 |
| key_import | 1 | 0 | 0 | 1 |
| ota_commit | 1 | 0 | 0 | 1 |

---

*Refreshed audit. See `docs/MCDC_GAPS.csv` for the full per-decision
table including line numbers and decision excerpts.*
