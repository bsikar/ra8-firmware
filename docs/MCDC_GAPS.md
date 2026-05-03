# MC/DC Coverage Gap Audit

Static audit of compound boolean decisions across first-party
sources in `libs/`, `src/`, and `port/` (excluding
`libs/third_party/`). Refreshed against the current test suite to
mark which decisions already have `test_mcdc_*` coverage.

## Methodology

- A *compound decision* is any `if`/`while`/`for` predicate (or
  bare line) whose joined text contains 2+ logical operators
  (`&&` / `||`) after stripping comments and string literals.
  Condition count = operators + 1.
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
- Total compound decisions found: **96**
- Decisions with at least one MC/DC test (`yes`): **11**
- Decisions in a partially-covered module (`partial`): **79**
- Decisions with no MC/DC test (`no`, gap): **6**
- Coverage rate (yes / total): **11.5%**

## Per-module gap counts (full table)

Sorted by uncovered+partial count (descending).

| Module | Total | Covered | Partial | Uncovered |
|--------|------:|--------:|--------:|----------:|
| ra_fs_fat | 7 | 1 | 6 | 0 |
| ra_etha | 5 | 0 | 5 | 0 |
| ra_rsip | 5 | 0 | 5 | 0 |
| ra_usb_paud | 5 | 0 | 5 | 0 |
| ra_vin | 5 | 0 | 5 | 0 |
| ra_dotf | 4 | 0 | 4 | 0 |
| ra_gfx_text | 4 | 0 | 4 | 0 |
| ra_ssie | 4 | 0 | 4 | 0 |
| ra_ble_gatt_client | 3 | 0 | 3 | 0 |
| ra_epub_chapter | 3 | 0 | 3 | 0 |
| ra_jpeg_sw | 3 | 0 | 3 | 0 |
| ra_touch_cal | 3 | 0 | 3 | 0 |
| ra_psa_crypto | 7 | 5 | 2 | 0 |
| mbedtls_aes_alt | 2 | 0 | 0 | 2 |
| nx_crypto_aes_alt | 2 | 0 | 0 | 2 |
| ra_ble_l2cap | 2 | 0 | 2 | 0 |
| ra_epaper | 2 | 0 | 2 | 0 |
| ra_net_udp | 2 | 0 | 2 | 0 |
| ra_usb_hmsc | 2 | 0 | 2 | 0 |
| ra_usb_pal | 2 | 0 | 2 | 0 |
| ra_usb_phid | 2 | 0 | 2 | 0 |
| ra_usb_pvnd | 2 | 0 | 2 | 0 |
| ra_modem_at | 2 | 1 | 1 | 0 |
| mbedtls_sha256_alt | 1 | 0 | 0 | 1 |
| nx_ether_driver_ra_eth | 1 | 0 | 0 | 1 |
| ra_ble | 1 | 0 | 1 | 0 |
| ra_ble_gatt | 1 | 0 | 1 | 0 |
| ra_flash | 1 | 0 | 1 | 0 |
| ra_i3c | 1 | 0 | 1 | 0 |
| ra_mipi_dsi | 1 | 0 | 1 | 0 |
| ra_mipi_phy | 1 | 0 | 1 | 0 |
| ra_ofs | 1 | 0 | 1 | 0 |
| ra_reflow_layout | 1 | 0 | 1 | 0 |
| ra_sci | 1 | 0 | 1 | 0 |
| ra_usb | 1 | 0 | 1 | 0 |
| ra_usb_composite | 1 | 0 | 1 | 0 |
| ra_usb_hhid | 1 | 0 | 1 | 0 |
| lx_nor_driver_ra_xspi | 2 | 2 | 0 | 0 |
| ra_net_ipv4 | 1 | 1 | 0 | 0 |
| ra_ota | 1 | 1 | 0 | 0 |

## Top 30 modules with at least one uncovered decision

| Module | Uncovered | Partial | Covered | Total |
|--------|----------:|--------:|--------:|------:|
| mbedtls_aes_alt | 2 | 0 | 0 | 2 |
| nx_crypto_aes_alt | 2 | 0 | 0 | 2 |
| mbedtls_sha256_alt | 1 | 0 | 0 | 1 |
| nx_ether_driver_ra_eth | 1 | 0 | 0 | 1 |

---

*Refreshed audit. See `docs/MCDC_GAPS.csv` for the full per-decision
table including line numbers and decision excerpts.*
